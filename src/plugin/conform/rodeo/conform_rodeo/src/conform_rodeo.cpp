// SPDX-License-Identifier: Apache-2.0
/**
 * Rodeo Smart Conform Engine for xStudio
 *
 * This plugin implements a high-performance conform engine for RodeoFX timelines:
 * - C++ handles performance-critical work: timeline parsing, batch queries, clip creation
 * - Python data source handles ShotGrid API calls
 *
 * Architecture:
 * 1. C++ parses timeline OTIO → extracts shot structure
 * 2. C++ deduplicates shot codes → builds batch query
 * 3. C++ sends single request to Python data source (RDOSHOTGRID)
 * 4. Python executes ShotGrid query → returns versions_by_shot
 * 5. C++ creates clips with metadata and populates track
 *
 * Request/Response format:
 *   C++ → Python: {operation, project_code, shot_codes[], department, status_list}
 *   Python → C++: {success, versions_by_shot: {shot → version_data}, missing_shots[]}
 */

#include <caf/actor_registry.hpp>
#include <caf/policy/select_all.hpp>
#include <regex>
#include <algorithm>
#include <set>
#include <filesystem>

#include "xstudio/conform/conformer.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/string_helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/timeline/track_actor.hpp"
#include "xstudio/timeline/gap_actor.hpp"
#include "xstudio/timeline/clip_actor.hpp"
#include "xstudio/media/media_actor.hpp"
#include "xstudio/data_source/data_source.hpp"
#include "xstudio/json_store/json_store_helper.hpp"

using namespace xstudio;
using namespace xstudio::conform;
using namespace xstudio::utility;
using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

// Task name prefix to identify Rodeo conform tasks
const std::string RODEO_TASK_PREFIX = "Rodeo: ";

// JSON pointers for Rodeo metadata paths
const auto shotgrid_shot_code_ptr =
    nlohmann::json::json_pointer("/shotgrid/shot_code");
const auto shotgrid_project_code_ptr =
    nlohmann::json::json_pointer("/shotgrid/project_code");
const auto shotgrid_version_id_ptr =
    nlohmann::json::json_pointer("/shotgrid/version_id");
const auto shotgrid_department_ptr =
    nlohmann::json::json_pointer("/shotgrid/department");
const auto shotgrid_status_ptr =
    nlohmann::json::json_pointer("/shotgrid/status");

// External metadata paths (set by media hook)
const auto external_show_ptr =
    nlohmann::json::json_pointer("/metadata/external/RodeoFX/show");
const auto external_shot_ptr =
    nlohmann::json::json_pointer("/metadata/external/RodeoFX/shot");

// Clip metadata paths
const auto clip_shot_ptr =
    nlohmann::json::json_pointer("/metadata/external/Rodeo/shot");
const auto clip_show_ptr =
    nlohmann::json::json_pointer("/metadata/external/Rodeo/show");

/**
 * Extract project/show code from metadata.
 */
std::string get_project_name(const JsonStore &metadata) {
    try {
        if (metadata.contains(shotgrid_project_code_ptr) &&
            !metadata.at(shotgrid_project_code_ptr).is_null()) {
            return metadata.at(shotgrid_project_code_ptr).get<std::string>();
        }
    } catch (...) {}

    try {
        if (metadata.contains(external_show_ptr) &&
            !metadata.at(external_show_ptr).is_null()) {
            return metadata.at(external_show_ptr).get<std::string>();
        }
    } catch (...) {}

    try {
        if (metadata.contains(clip_show_ptr) && !metadata.at(clip_show_ptr).is_null()) {
            return metadata.at(clip_show_ptr).get<std::string>();
        }
    } catch (...) {}

    return "";
}

/**
 * Extract shot code from metadata.
 */
std::string get_shot_name(const JsonStore &metadata) {
    try {
        if (metadata.contains(shotgrid_shot_code_ptr) &&
            !metadata.at(shotgrid_shot_code_ptr).is_null()) {
            return metadata.at(shotgrid_shot_code_ptr).get<std::string>();
        }
    } catch (...) {}

    try {
        if (metadata.contains(external_shot_ptr) &&
            !metadata.at(external_shot_ptr).is_null()) {
            return metadata.at(external_shot_ptr).get<std::string>();
        }
    } catch (...) {}

    try {
        if (metadata.contains(clip_shot_ptr) && !metadata.at(clip_shot_ptr).is_null()) {
            return metadata.at(clip_shot_ptr).get<std::string>();
        }
    } catch (...) {}

    return "";
}

/**
 * Extract show code from timeline name.
 * Expected format: show_sequence_... (e.g., "drb_206044_cut_context_Comp")
 */
std::string extract_show_from_timeline_name(const std::string &timeline_name) {
    if (timeline_name.empty())
        return "";

    auto pos = timeline_name.find('_');
    if (pos != std::string::npos) {
        return timeline_name.substr(0, pos);
    }
    return timeline_name;
}

/**
 * Extract shot code from clip name using regex patterns.
 * Handles formats like: 206044_0010.comp.v7, 206044_0010_comp_v001
 */
std::string extract_shot_from_clip_name(const std::string &name) {
    if (name.empty())
        return "";

    // Pattern: SEQ_SHOT (e.g., 206044_0010)
    static const std::regex seq_shot_re(R"(^(\d{6}_\d{4}))");
    std::smatch match;
    if (std::regex_search(name, match, seq_shot_re)) {
        return match[1].str();
    }

    // Pattern: seq###_shot#### or similar
    static const std::regex generic_shot_re(R"(^([a-zA-Z]+\d+_[a-zA-Z]*\d+))");
    if (std::regex_search(name, match, generic_shot_re)) {
        return match[1].str();
    }

    return "";
}

} // anonymous namespace

/**
 * Shot structure item - represents a clip or gap in the timeline
 */
struct ShotStructureItem {
    enum Type { CLIP, GAP };
    Type type;
    std::string shot_code;
    FrameRange source_range;
    std::string clip_name;
    Uuid original_clip_uuid;  // For tracking original clip
};

/**
 * Preset structure for conform operations
 */
struct ConformPreset {
    std::string id;
    std::string menu_text;
    std::string track_name;
    std::string department;  // empty = any department
    std::vector<std::string> status_list;  // ["all"] = any status

    static ConformPreset from_json(const nlohmann::json &j) {
        ConformPreset p;
        p.id = j.value("id", "");
        p.menu_text = j.value("menu_text", "");
        p.track_name = j.value("track_name", "");
        p.department = j.value("department", "");
        if (j.contains("status_list") && j["status_list"].is_array()) {
            for (const auto &s : j["status_list"]) {
                if (s.is_string()) {
                    p.status_list.push_back(s.get<std::string>());
                }
            }
        }
        return p;
    }
};

/**
 * RodeoConform - Main conformer class for Rodeo timelines.
 */
class RodeoConform : public Conformer {
  public:
    RodeoConform(const utility::JsonStore &prefs = utility::JsonStore()) : Conformer(prefs) {
        update_preferences(prefs);
    }
    ~RodeoConform() = default;

    void update_preferences(const utility::JsonStore &prefs) override {
        try {
            reuse_media_ = global_store::preference_value<bool>(
                prefs, "/plugin/conformer/rodeo/reuse_media");
        } catch (...) {
            reuse_media_ = true;
        }

        // Load presets from /core/conform/presets
        try {
            auto presets_json = global_store::preference_value<JsonStore>(
                prefs, "/core/conform/presets");

            presets_.clear();
            task_names_.clear();

            if (presets_json.is_array()) {
                for (const auto &p : presets_json) {
                    auto preset = ConformPreset::from_json(p);
                    if (!preset.menu_text.empty()) {
                        presets_[preset.menu_text] = preset;
                        // Add with Rodeo prefix for identification
                        task_names_.push_back(RODEO_TASK_PREFIX + preset.menu_text);
                    }
                }
            }
            spdlog::info(
                "RodeoConform: Loaded {} presets from /core/conform/presets",
                presets_.size());
        } catch (const std::exception &err) {
            spdlog::debug("RodeoConform: Could not load presets: {}", err.what());
        }

        // If no presets loaded, add some defaults
        if (presets_.empty()) {
            spdlog::info("RodeoConform: Using default presets");
            add_default_presets();
        }
    }

    /**
     * Return list of conform tasks.
     * These appear in the Replace/Compare menus.
     */
    std::vector<std::string> conform_tasks() override {
        if (task_names_.empty()) {
            spdlog::debug("RodeoConform: No tasks available");
        }
        return task_names_;
    }

    /**
     * Get preset by task name (strips "Rodeo: " prefix)
     */
    std::optional<ConformPreset> get_preset(const std::string &task_name) const {
        std::string name = task_name;
        // Strip prefix if present
        if (starts_with(name, RODEO_TASK_PREFIX)) {
            name = name.substr(RODEO_TASK_PREFIX.length());
        }
        auto it = presets_.find(name);
        if (it != presets_.end()) {
            return it->second;
        }
        return {};
    }

    [[nodiscard]] bool reuse_media() const { return reuse_media_; }

  private:
    void add_default_presets() {
        // Add some default presets for testing
        std::vector<std::pair<std::string, ConformPreset>> defaults = {
            {"Comp: Latest", {"comp_latest", "Comp: Latest", "Comp: Latest", "Comp", {"all"}}},
            {"Comp: Delivered", {"comp_delivered", "Comp: Delivered", "Comp: Delivered", "Comp", {"dlvr", "cfin"}}},
            {"Anim: Latest", {"anim_latest", "Anim: Latest", "Anim: Latest", "Anim", {"all"}}},
            {"Anim: Pushed", {"anim_pushed", "Anim: Pushed", "Anim: Pushed", "Anim", {"push", "apr"}}},
        };

        for (const auto &[name, preset] : defaults) {
            presets_[name] = preset;
            task_names_.push_back(RODEO_TASK_PREFIX + name);
        }
    }

    std::map<std::string, ConformPreset> presets_;
    std::vector<std::string> task_names_;
    bool reuse_media_{true};
};

/**
 * RodeoConformActor - CAF actor wrapping RodeoConform with smart engine.
 */
template <typename T>
class RodeoConformActor : public caf::event_based_actor {
  public:
    RodeoConformActor(
        caf::actor_config &cfg, const utility::JsonStore &prefs = utility::JsonStore())
        : caf::event_based_actor(cfg), conform_(prefs) {
        spdlog::info("RodeoConformActor (smart engine) created");
        utility::print_on_exit(this, "RodeoConformActor");

        // Join global store broadcast to receive preference updates
        {
            auto prefs = global_store::GlobalStoreHelper(system());
            utility::JsonStore js;
            utility::join_broadcast(this, prefs.get_group(js));
            conform_.update_preferences(js);
        }

        behavior_.assign(
            [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {},

            // Basic conform: match media to clips by show/shot
            [=](conform_atom, const ConformRequest &crequest) -> result<ConformReply> {
                auto rp = make_response_promise<ConformReply>();
                conform_request(rp, crequest);
                return rp;
            },

            // Task-based conform (e.g., "Rodeo: Comp: Latest")
            // This is the SMART ENGINE entry point
            [=](conform_atom,
                const std::string &conform_task,
                const ConformRequest &crequest) -> result<ConformReply> {
                spdlog::info("RodeoConformActor: Smart engine conform_task_request for '{}'", conform_task);
                auto rp = make_response_promise<ConformReply>();
                conform_task_request(rp, conform_task, crequest);
                return rp;
            },

            // Prepare timeline for conform
            [=](conform_atom,
                const UuidActor &timeline,
                const bool only_create_conform_track) -> result<bool> {
                spdlog::info("RodeoConformActor: prepare_timeline");
                auto rp = make_response_promise<bool>();
                prepare_timeline(rp, timeline, only_create_conform_track);
                return rp;
            },

            // Find sequences related to media
            // NOTE: Disabled for smart engine - we handle everything in C++
            [=](conform_atom,
                const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &media)
                -> result<std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>> {
                // Return empty results - sequence lookup not needed for Rodeo workflow
                auto result = std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>();
                result.reserve(media.size());
                for (size_t i = 0; i < media.size(); ++i) {
                    result.push_back({});
                }
                spdlog::debug(
                    "RodeoConform: conform_find_timeline returning empty (smart engine handles conform)");
                return result;
            },

            // Find matching media by key
            [=](conform_atom,
                const std::string &key,
                const std::pair<utility::UuidActor, utility::JsonStore> &needle,
                const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &haystack)
                -> result<utility::UuidActorVector> {
                auto rp = make_response_promise<utility::UuidActorVector>();
                find_matching(rp, key, needle, haystack);
                return rp;
            },

            // Return available conform tasks
            [=](conform_tasks_atom) -> std::vector<std::string> {
                auto tasks = conform_.conform_tasks();
                spdlog::info("RodeoConformActor: returning {} tasks", tasks.size());
                for (const auto &t : tasks) {
                    spdlog::debug("  - {}", t);
                }
                return tasks;
            },

            // Handle preference updates
            [=](json_store::update_atom,
                const utility::JsonStore & /*change*/,
                const std::string & /*path*/,
                const utility::JsonStore &full) {
                return mail(json_store::update_atom_v, full)
                    .delegate(actor_cast<caf::actor>(this));
            },

            [=](json_store::update_atom, const utility::JsonStore &js) {
                try {
                    conform_.update_preferences(js);
                } catch (const std::exception &err) {
                    spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
                }
            });
    }

    ~RodeoConformActor() override = default;
    caf::behavior make_behavior() override { return behavior_; }

  private:
    // ========================================================================
    // SMART ENGINE METHODS
    // ========================================================================

    /**
     * Extract shot structure from timeline conform track.
     *
     * Walks through the conform track and extracts:
     * - Shot codes from clip names/metadata
     * - Source ranges for proper timing
     * - Gap positions for track structure
     */
    std::vector<ShotStructureItem> extract_shot_structure(
        const timeline::Item &timeline_item,
        const Uuid &conform_track_uuid,
        const std::map<Uuid, JsonStore> &metadata) {

        std::vector<ShotStructureItem> structure;

        // Find the conform track
        auto tracks = timeline_item.find_all_items(timeline::IT_VIDEO_TRACK);
        const timeline::Item *conform_track = nullptr;

        for (const auto &track_ref : tracks) {
            if (track_ref.get().uuid() == conform_track_uuid) {
                conform_track = &track_ref.get();
                break;
            }
        }

        if (!conform_track) {
            spdlog::warn("RodeoConform: Could not find conform track {}", to_string(conform_track_uuid));
            return structure;
        }

        // Extract structure from conform track children
        for (const auto &item : conform_track->children()) {
            ShotStructureItem si;

            if (item.item_type() == timeline::IT_GAP) {
                si.type = ShotStructureItem::GAP;
                si.shot_code = "";
                si.clip_name = "Gap";
            } else if (item.item_type() == timeline::IT_CLIP) {
                si.type = ShotStructureItem::CLIP;
                si.clip_name = item.name();
                si.original_clip_uuid = item.uuid();

                // Try to get shot code from metadata first
                auto shot_code = std::string();
                if (metadata.count(item.uuid())) {
                    shot_code = get_shot_name(metadata.at(item.uuid()));
                }

                // Fallback: parse from clip name
                if (shot_code.empty()) {
                    shot_code = extract_shot_from_clip_name(item.name());
                }

                // Last resort: use clip name if it looks like a shot
                if (shot_code.empty()) {
                    static const std::regex valid_shot_re(R"(^[a-zA-Z0-9_]+$)");
                    if (std::regex_match(item.name(), valid_shot_re)) {
                        shot_code = item.name();
                    }
                }

                si.shot_code = shot_code;
            } else {
                continue;  // Skip other item types
            }

            // Copy the source range
            if (item.active_range()) {
                si.source_range = *item.active_range();
            } else {
                si.source_range = item.trimmed_range();
            }

            structure.push_back(si);
        }

        spdlog::info("RodeoConform: Extracted {} items from conform track", structure.size());
        return structure;
    }

    /**
     * Deduplicate shot codes from structure.
     */
    std::vector<std::string> deduplicate_shots(const std::vector<ShotStructureItem> &structure) {
        std::set<std::string> unique_shots;
        for (const auto &item : structure) {
            if (item.type == ShotStructureItem::CLIP && !item.shot_code.empty()) {
                unique_shots.insert(item.shot_code);
            }
        }
        return std::vector<std::string>(unique_shots.begin(), unique_shots.end());
    }

    /**
     * Build batch query JSON for Python data source.
     */
    JsonStore build_batch_query(
        const std::string &project_code,
        const std::vector<std::string> &shot_codes,
        const ConformPreset &preset) {

        auto query = R"({
            "operation": "BatchQueryVersions",
            "project_code": "",
            "shot_codes": [],
            "department": "",
            "status_list": []
        })"_json;

        query["project_code"] = project_code;
        query["shot_codes"] = shot_codes;
        query["department"] = preset.department;
        query["status_list"] = preset.status_list;

        return query;
    }

    /**
     * Registry name for the Rodeo ShotGrid data source.
     */
    static constexpr const char* RDOSHOTGRID_REGISTRY = "RDOSHOTGRID";

    /**
     * Get the Rodeo ShotGrid data source actor from registry.
     * The data source is a C++ bridge that communicates with the Python plugin.
     */
    caf::actor get_data_source() {
        if (!data_source_) {
            spdlog::info("RodeoConformActor: Looking up '{}' data source in registry", RDOSHOTGRID_REGISTRY);
            data_source_ = system().registry().template get<caf::actor>(RDOSHOTGRID_REGISTRY);
            if (data_source_) {
                spdlog::info("RodeoConformActor: Found RDOSHOTGRID data source");
            } else {
                spdlog::warn("RodeoConformActor: RDOSHOTGRID data source NOT found in registry");
            }
        }
        return data_source_;
    }

    /**
     * Send batch query to the Python data source and process response.
     */
    void send_batch_query(
        caf::typed_response_promise<ConformReply> rp,
        const JsonStore &query,
        const ConformRequest &crequest,
        const std::vector<ShotStructureItem> &structure,
        const ConformPreset &preset) {

        auto data_source = get_data_source();
        if (!data_source) {
            spdlog::warn("RodeoConformActor: RDOSHOTGRID data source not found, falling back to basic conform");
            conform_request(rp, crequest);
            return;
        }

        spdlog::info("RodeoConformActor: Sending batch query to RDOSHOTGRID data source");

        // Store shared state for the callback
        auto structure_ptr = std::make_shared<std::vector<ShotStructureItem>>(structure);
        auto preset_ptr = std::make_shared<ConformPreset>(preset);
        auto crequest_ptr = std::make_shared<ConformRequest>(crequest);

        mail(data_source::get_data_atom_v, query)
            .request(data_source, std::chrono::seconds(60))
            .then(
                [=, this](const JsonStore &response) mutable {
                    spdlog::info("RodeoConformActor: Received response from data source");
                    process_batch_response(rp, response, *crequest_ptr, *structure_ptr, *preset_ptr);
                },
                [=, this](const caf::error &err) mutable {
                    spdlog::warn("RodeoConformActor: Data source error: {}", to_string(err));
                    // Fall back to basic conform on error
                    conform_request(rp, *crequest_ptr);
                });
    }

    /**
     * Process batch response from Python data source.
     * Creates media actors and builds ConformReply for track population.
     */
    void process_batch_response(
        caf::typed_response_promise<ConformReply> rp,
        const JsonStore &response,
        const ConformRequest &crequest,
        const std::vector<ShotStructureItem> &structure,
        const ConformPreset &preset) {

        try {
            // Check for error response
            if (!response.value("success", false)) {
                auto error_msg = response.value("error", std::string("Unknown error"));
                spdlog::warn("RodeoConformActor: Batch query failed: {}", error_msg);
                conform_request(rp, crequest);
                return;
            }

            // Get versions_by_shot from response
            if (!response.contains("versions_by_shot")) {
                spdlog::warn("RodeoConformActor: Response missing versions_by_shot");
                conform_request(rp, crequest);
                return;
            }

            auto versions_by_shot = response.at("versions_by_shot");
            auto missing_shots = response.value("missing_shots", std::vector<std::string>());

            spdlog::info(
                "RodeoConformActor: Got {} versions, {} missing shots",
                versions_by_shot.size(),
                missing_shots.size());

            // Log missing shots
            for (const auto &shot : missing_shots) {
                spdlog::debug("RodeoConformActor: Missing shot: {}", shot);
            }

            // Create media actors from version paths
            std::map<std::string, UuidActor> media_by_shot;
            auto playlist = crequest.container_.actor();

            for (const auto &[shot, version_data] : versions_by_shot.items()) {
                auto version_code = version_data.value("version_code", std::string());
                auto path_to_movie = version_data.value("sg_path_to_movie", std::string());
                auto path_to_frames = version_data.value("sg_path_to_frames", std::string());

                // Prefer movie path, fallback to frames
                auto media_path = path_to_movie.empty() ? path_to_frames : path_to_movie;

                if (media_path.empty()) {
                    spdlog::debug("RodeoConformActor: No path for shot {}", shot);
                    continue;
                }

                // Check if file exists
                if (!fs::exists(media_path)) {
                    spdlog::debug("RodeoConformActor: Path not found for shot {}: {}", shot, media_path);
                    continue;
                }

                try {
                    // Create media from path
                    auto uri = posix_path_to_uri(media_path);
                    auto media_uuid = Uuid::generate();
                    auto source_uuid = Uuid::generate();

                    // Get file extension for source name
                    auto ext = ltrim_char(
                        to_upper(fs::path(media_path).extension().string()), '.');

                    // Create media source actor
                    auto source = spawn<media::MediaSourceActor>(
                        ext.empty() ? "UNKNOWN" : ext,
                        uri,
                        FrameRate(timebase::k_flicks_24fps),
                        source_uuid);

                    // Create media actor wrapping the source
                    auto media_actor = spawn<media::MediaActor>(
                        version_code,
                        media_uuid,
                        UuidActorVector({UuidActor(source_uuid, source)}));

                    // Build ShotGrid metadata for the media
                    auto sg_meta = R"({"metadata": {"shotgrid": {}}})"_json;
                    sg_meta["metadata"]["shotgrid"]["version_id"] = version_data.value("version_id", 0);
                    sg_meta["metadata"]["shotgrid"]["version_code"] = version_code;
                    sg_meta["metadata"]["shotgrid"]["shot_code"] = shot;
                    sg_meta["metadata"]["shotgrid"]["shot_id"] = version_data.value("shot_id", 0);
                    sg_meta["metadata"]["shotgrid"]["sg_department"] = version_data.value("sg_department", "");
                    sg_meta["metadata"]["shotgrid"]["sg_status_list"] = version_data.value("sg_status_list", "");

                    // Set metadata on media actor
                    anon_mail(json_store::set_json_atom_v, JsonStore(sg_meta)).send(media_actor);

                    media_by_shot[shot] = UuidActor(media_uuid, media_actor);

                    spdlog::info(
                        "RodeoConformActor: Created media for shot {}: {} ({})",
                        shot, version_code, media_path);

                } catch (const std::exception &err) {
                    spdlog::warn(
                        "RodeoConformActor: Failed to create media for shot {}: {}",
                        shot, err.what());
                }
            }

            spdlog::info(
                "RodeoConformActor: Created {} media actors for {} structure items",
                media_by_shot.size(), structure.size());

            // Build ConformReply matching clips to media
            auto creply = ConformReply(crequest);

            // For each item in the request, find matching media
            for (const auto &req_item : crequest.items_) {
                auto shot_code = std::string();

                // Try to get shot code from clip metadata
                if (crequest.metadata_.count(req_item.item_.uuid())) {
                    shot_code = get_shot_name(crequest.metadata_.at(req_item.item_.uuid()));
                }

                // Fallback: parse from clip name
                if (shot_code.empty() && !req_item.clip_.name().empty()) {
                    shot_code = extract_shot_from_clip_name(req_item.clip_.name());
                }

                if (!shot_code.empty() && media_by_shot.count(shot_code)) {
                    // Found matching media - add to reply
                    auto ritems = std::vector<ConformReplyItem>();
                    ritems.emplace_back(std::make_tuple(media_by_shot.at(shot_code)));
                    creply.items_.push_back(ritems);
                    spdlog::debug("RodeoConformActor: Matched clip {} to shot {}", req_item.clip_.name(), shot_code);
                } else {
                    // No match - return empty
                    creply.items_.push_back({});
                    spdlog::debug("RodeoConformActor: No match for clip {} (shot={})", req_item.clip_.name(), shot_code);
                }
            }

            // Set operations to tell conform system to create and insert media
            creply.operations_["create_media"] = true;
            creply.operations_["insert_media"] = true;

            spdlog::info(
                "RodeoConformActor: Delivering ConformReply with {} items",
                creply.items_.size());

            rp.deliver(creply);

        } catch (const std::exception &err) {
            spdlog::warn("RodeoConformActor: Error processing batch response: {}", err.what());
            conform_request(rp, crequest);
        }
    }

    /**
     * SMART ENGINE: Handle task-based conform request.
     *
     * This is the main entry point for the smart engine:
     * 1. Extract shot structure from timeline
     * 2. Build batch query
     * 3. Send to Python data source
     * 4. Process response and populate track
     */
    void conform_task_request(
        caf::typed_response_promise<ConformReply> rp,
        const std::string &conform_task,
        const ConformRequest &crequest) {

        try {
            spdlog::info(
                "RodeoConformActor: Smart engine processing task '{}' with {} items",
                conform_task,
                crequest.items_.size());

            if (crequest.items_.empty()) {
                spdlog::info("RodeoConformActor: No items to conform");
                rp.deliver(ConformReply(crequest));
                return;
            }

            auto preset = conform_.get_preset(conform_task);
            if (!preset) {
                spdlog::warn("RodeoConformActor: Unknown task '{}'", conform_task);
                rp.deliver(ConformReply(crequest));
                return;
            }

            spdlog::info(
                "RodeoConformActor: Using preset '{}' (dept={}, status={})",
                preset->menu_text,
                preset->department.empty() ? "any" : preset->department,
                preset->status_list.empty() ? "any" : preset->status_list[0]);

            // Check if we have template tracks (conform track structure)
            spdlog::info(
                "RodeoConformActor: template_tracks={}, metadata entries={}",
                crequest.template_tracks_.size(),
                crequest.metadata_.size());

            // If template_tracks_ is empty, try to detect conform track from timeline
            timeline::Item template_track(timeline::IT_NONE);

            if (crequest.template_tracks_.empty()) {
                spdlog::info("RodeoConformActor: No template tracks provided, attempting to detect conform track from timeline");

                // Try to get the conform track from the timeline's properties
                scoped_actor sys{system()};
                try {
                    // Get the timeline item from the container
                    auto timeline_item = request_receive<timeline::Item>(
                        *sys, crequest.container_.actor(), timeline::item_atom_v);

                    // Check if conform_track_uuid is set in timeline properties
                    auto tprop = timeline_item.prop();
                    if (tprop.contains("conform_track_uuid") && !tprop.at("conform_track_uuid").is_null()) {
                        auto conform_track_uuid = tprop.value("conform_track_uuid", Uuid());
                        if (!conform_track_uuid.is_null()) {
                            spdlog::info(
                                "RodeoConformActor: Found conform_track_uuid in timeline props: {}",
                                to_string(conform_track_uuid));

                            // Fetch the specific track item
                            template_track = request_receive<timeline::Item>(
                                *sys, crequest.container_.actor(), timeline::item_atom_v, conform_track_uuid);

                            spdlog::info(
                                "RodeoConformActor: Retrieved conform track '{}' with {} children",
                                template_track.name(),
                                template_track.children().size());
                        } else {
                            spdlog::warn("RodeoConformActor: conform_track_uuid is null");
                        }
                    } else {
                        spdlog::info("RodeoConformActor: No conform_track_uuid in timeline props, searching for 'Conform Track'");

                        // Fallback: look for a track named "Conform Track"
                        auto video_tracks = timeline_item.find_all_items(timeline::IT_VIDEO_TRACK);
                        for (const auto &track_ref : video_tracks) {
                            if (track_ref.get().name() == "Conform Track") {
                                template_track = track_ref.get();
                                spdlog::info(
                                    "RodeoConformActor: Found 'Conform Track' with {} children",
                                    template_track.children().size());
                                break;
                            }
                        }
                    }
                } catch (const std::exception &err) {
                    spdlog::warn("RodeoConformActor: Error detecting conform track: {}", err.what());
                }

                if (template_track.item_type() == timeline::IT_NONE) {
                    spdlog::warn("RodeoConformActor: Could not detect conform track, falling back to basic conform");
                    conform_request(rp, crequest);
                    return;
                }
            } else {
                // Use the provided template track
                template_track = crequest.template_tracks_.at(0);
            }

            auto conform_track_uuid = template_track.uuid();
            spdlog::info(
                "RodeoConformActor: Template track '{}' with {} children",
                template_track.name(),
                template_track.children().size());

            // Try to get project code from various sources
            auto project_code = std::string();

            // 1. Try to get from first item's metadata
            if (!crequest.items_.empty()) {
                auto item_uuid = crequest.items_.at(0).item_.uuid();
                spdlog::debug("RodeoConformActor: First item UUID: {}", to_string(item_uuid));
                if (crequest.metadata_.count(item_uuid)) {
                    project_code = get_project_name(crequest.metadata_.at(item_uuid));
                    spdlog::debug("RodeoConformActor: Project from first item: '{}'", project_code);
                } else {
                    spdlog::debug("RodeoConformActor: No metadata for first item");
                }
            }

            // 2. Fallback: try to extract from any metadata entry
            if (project_code.empty()) {
                spdlog::debug("RodeoConformActor: Searching all {} metadata entries for project", crequest.metadata_.size());
                for (const auto &[uuid, meta] : crequest.metadata_) {
                    project_code = get_project_name(meta);
                    if (!project_code.empty()) {
                        spdlog::debug("RodeoConformActor: Found project '{}' in metadata {}", project_code, to_string(uuid));
                        break;
                    }
                }
            }

            // 3. Fallback: try to extract from clip metadata in conform track
            if (project_code.empty()) {
                spdlog::debug("RodeoConformActor: Trying to get project from conform track clip metadata");
                auto clips = template_track.find_all_items(timeline::IT_CLIP);
                for (const auto &clip_ref : clips) {
                    auto clip_meta = clip_ref.get().prop();
                    project_code = get_project_name(clip_meta);
                    if (!project_code.empty()) {
                        spdlog::debug("RodeoConformActor: Found project '{}' in clip prop", project_code);
                        break;
                    }
                }
            }

            // 4. Fallback: try to extract from timeline properties or name
            if (project_code.empty()) {
                spdlog::debug("RodeoConformActor: Trying to get project from timeline");
                scoped_actor sys{system()};
                try {
                    auto timeline_item = request_receive<timeline::Item>(
                        *sys, crequest.container_.actor(), timeline::item_atom_v);

                    // Try timeline path first
                    static const auto SHOW_REGEX = std::regex(R"(^(?:/rdo)?/shows/([^/]+)/.+$)");
                    auto timeline_path = timeline_item.prop().value("path", std::string());
                    if (!timeline_path.empty()) {
                        std::cmatch m;
                        auto uri_path = caf::make_uri(timeline_path);
                        if (uri_path) {
                            auto posix_path = uri_to_posix_path(*uri_path);
                            if (std::regex_match(posix_path.c_str(), m, SHOW_REGEX)) {
                                project_code = m[1];
                                spdlog::debug("RodeoConformActor: Extracted project '{}' from timeline path", project_code);
                            }
                        }
                    }

                    // Try timeline name
                    if (project_code.empty()) {
                        project_code = extract_show_from_timeline_name(timeline_item.name());
                        if (!project_code.empty()) {
                            spdlog::debug("RodeoConformActor: Extracted project '{}' from timeline name", project_code);
                        }
                    }
                } catch (const std::exception &err) {
                    spdlog::debug("RodeoConformActor: Error getting timeline for project: {}", err.what());
                }
            }

            if (project_code.empty()) {
                spdlog::warn("RodeoConformActor: Could not determine project code from any source");
                conform_request(rp, crequest);
                return;
            }

            spdlog::info("RodeoConformActor: Project code: {}", project_code);

            // Extract shot structure from the template track
            // We need to build a temporary timeline item to use extract_shot_structure
            // For now, use the clips from template_tracks_
            std::vector<ShotStructureItem> structure;
            auto clips = template_track.find_all_items(timeline::IT_CLIP);

            for (const auto &clip_ref : clips) {
                const auto &clip = clip_ref.get();
                ShotStructureItem si;
                si.type = ShotStructureItem::CLIP;
                si.clip_name = clip.name();
                si.original_clip_uuid = clip.uuid();

                // Get shot code from metadata or clip name
                auto shot_code = std::string();
                if (crequest.metadata_.count(clip.uuid())) {
                    shot_code = get_shot_name(crequest.metadata_.at(clip.uuid()));
                }
                if (shot_code.empty()) {
                    shot_code = extract_shot_from_clip_name(clip.name());
                }
                if (shot_code.empty() && !clip.name().empty()) {
                    static const std::regex valid_shot_re(R"(^[a-zA-Z0-9_]+$)");
                    if (std::regex_match(clip.name(), valid_shot_re)) {
                        shot_code = clip.name();
                    }
                }

                si.shot_code = shot_code;

                if (clip.active_range()) {
                    si.source_range = *clip.active_range();
                } else {
                    si.source_range = clip.trimmed_range();
                }

                structure.push_back(si);
            }

            // Also add gaps
            auto all_items = template_track.children();
            for (const auto &item : all_items) {
                if (item.item_type() == timeline::IT_GAP) {
                    ShotStructureItem si;
                    si.type = ShotStructureItem::GAP;
                    si.shot_code = "";
                    si.clip_name = "Gap";
                    if (item.active_range()) {
                        si.source_range = *item.active_range();
                    } else {
                        si.source_range = item.trimmed_range();
                    }
                    structure.push_back(si);
                }
            }

            spdlog::info("RodeoConformActor: Extracted {} structure items", structure.size());

            // Deduplicate shot codes
            auto shot_codes = deduplicate_shots(structure);
            if (shot_codes.empty()) {
                spdlog::warn("RodeoConformActor: No shot codes found in structure");
                conform_request(rp, crequest);
                return;
            }

            spdlog::info("RodeoConformActor: {} unique shots to query", shot_codes.size());

            // Build batch query
            auto query = build_batch_query(project_code, shot_codes, *preset);
            spdlog::info("RodeoConformActor: Built batch query for {} shots", shot_codes.size());

            // Send query to Python data source via the C++ bridge
            send_batch_query(rp, query, crequest, structure, *preset);

        } catch (const std::exception &err) {
            spdlog::warn("RodeoConformActor: conform_task_request error: {}", err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

    /**
     * Handle basic conform request - match media to clips by show/shot.
     */
    void conform_request(
        caf::typed_response_promise<ConformReply> rp, const ConformRequest &crequest) {
        try {
            auto creply = ConformReply(crequest);

            if (crequest.template_tracks_.empty()) {
                spdlog::debug("RodeoConformActor: No template tracks");
                rp.deliver(creply);
                return;
            }

            auto clips = crequest.template_tracks_.at(0).find_all_items(timeline::IT_CLIP);
            spdlog::info("RodeoConformActor: Found {} clips in template track", clips.size());

            // Build clip lookup maps
            std::map<Uuid, std::string> clip_project_map;
            std::map<Uuid, std::string> clip_shot_map;

            for (const auto &c : clips) {
                auto clip_uuid  = c.get().uuid();
                auto media_uuid = c.get().prop().value("media_uuid", Uuid());

                auto clip_project = get_project_name(crequest.metadata_.at(clip_uuid));
                auto clip_shot    = get_shot_name(crequest.metadata_.at(clip_uuid));

                // Fallback to media metadata
                if (clip_project.empty() && !media_uuid.is_null() &&
                    crequest.metadata_.count(media_uuid)) {
                    clip_project = get_project_name(crequest.metadata_.at(media_uuid));
                }
                if (clip_shot.empty() && !media_uuid.is_null() &&
                    crequest.metadata_.count(media_uuid)) {
                    clip_shot = get_shot_name(crequest.metadata_.at(media_uuid));
                }

                clip_project_map[clip_uuid] = clip_project;
                clip_shot_map[clip_uuid]    = clip_shot;
            }

            std::set<utility::Uuid> matched_clips;
            const auto only_one_match =
                crequest.operations_.value("only_one_clip_match", false);

            auto clip_track_uuid = Uuid();

            // Match media to clips
            for (const auto &i : crequest.items_) {
                if (clip_track_uuid != i.clip_track_uuid_) {
                    matched_clips.clear();
                    clip_track_uuid = i.clip_track_uuid_;
                }

                if (clips.empty()) {
                    creply.items_.push_back({});
                } else {
                    try {
                        const auto meta = crequest.metadata_.at(i.item_.uuid());
                        auto project    = get_project_name(meta);
                        auto shot       = get_shot_name(meta);

                        if (project.empty() || shot.empty()) {
                            creply.items_.push_back({});
                            spdlog::debug(
                                "RodeoConformActor: Media missing metadata project='{}' shot='{}'",
                                project, shot);
                        } else {
                            auto ritems = std::vector<ConformReplyItem>();

                            for (const auto &c : clips) {
                                auto clip_uuid = c.get().uuid();
                                if ((!only_one_match || !matched_clips.count(clip_uuid)) &&
                                    clip_project_map.at(clip_uuid) == project &&
                                    clip_shot_map.at(clip_uuid) == shot) {
                                    ritems.push_back(std::make_tuple(c.get().uuid_actor()));
                                    matched_clips.insert(clip_uuid);
                                    spdlog::debug(
                                        "RodeoConformActor: Matched {} to clip {}",
                                        shot, c.get().name());
                                    if (only_one_match)
                                        break;
                                }
                            }

                            if (ritems.empty()) {
                                creply.items_.push_back({});
                            } else {
                                creply.items_.push_back(ritems);
                            }
                        }
                    } catch (const std::exception &err) {
                        spdlog::warn("RodeoConformActor: Error matching: {}", err.what());
                        creply.items_.push_back({});
                    }
                }
            }

            spdlog::info("RodeoConformActor: Matched {} items", creply.items_.size());
            rp.deliver(creply);
        } catch (const std::exception &err) {
            spdlog::warn("RodeoConformActor: conform_request error: {}", err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

    /**
     * Prepare timeline for conform operations.
     */
    void prepare_timeline(
        caf::typed_response_promise<bool> rp,
        const UuidActor &timeline,
        const bool only_create_conform_track) {

        static const auto SHOW_REGEX = std::regex(R"(^(?:/rdo)?/shows/([^/]+)/.+$)");

        scoped_actor sys{system()};
        try {
            spdlog::info("RodeoConformActor: prepare_timeline");

            auto found_project = std::string();
            auto timeline_item =
                request_receive<timeline::Item>(*sys, timeline.actor(), timeline::item_atom_v);

            // Extract project from timeline path
            auto timeline_path = timeline_item.prop().value("path", std::string());
            if (!timeline_path.empty()) {
                std::cmatch m;
                auto uri_path = caf::make_uri(timeline_path);
                if (uri_path) {
                    auto posix_path = uri_to_posix_path(*uri_path);
                    if (std::regex_match(posix_path.c_str(), m, SHOW_REGEX)) {
                        found_project = m[1];
                    }
                }
            }

            // Fallback: extract from timeline name
            if (found_project.empty()) {
                found_project = extract_show_from_timeline_name(timeline_item.name());
            }

            spdlog::info("RodeoConformActor: Found project '{}'", found_project);

            // Get video tracks
            auto video_tracks = timeline_item.find_all_items(timeline::IT_VIDEO_TRACK);
            auto insert_index = static_cast<int>(video_tracks.size());

            // Find bottom non-empty video track as conform track
            auto vtrack = timeline::Item(timeline::IT_NONE);
            std::reverse(video_tracks.begin(), video_tracks.end());

            for (const auto &i : video_tracks) {
                if (!i.get().empty()) {
                    vtrack = i.get();
                    break;
                }
            }

            if (vtrack.item_type() == timeline::IT_NONE) {
                spdlog::warn("RodeoConformActor: No non-empty video track found");
                rp.deliver(false);
                return;
            }

            vtrack.set_name("Conform Track");
            vtrack.set_locked(true);
            vtrack.set_enabled(false);

            // Process clips to extract shot metadata
            auto tframe = timeline_item.trimmed_start();

            for (auto &i : vtrack.children()) {
                if (i.item_type() == timeline::IT_CLIP) {
                    auto pm = R"({"metadata": {"external": {"Rodeo": {"shot": null, "show": null}}}})"_json;

                    // Try to get shot from clip name or metadata
                    auto shot = i.name();
                    auto is_valid = !shot.empty() && shot != "UNKNOWN";

                    // Validate shot name format
                    const static std::regex valid_shot_re(R"(^[a-zA-Z0-9_]+$)");
                    if (!std::regex_match(shot, valid_shot_re)) {
                        is_valid = false;
                    }

                    if (is_valid) {
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/show")] = found_project;
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/shot")] = shot;
                        i.set_flag("#FF00FF00");  // Green
                        i.set_enabled(true);
                    } else {
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/show")] =
                            found_project.empty() ? "UNKNOWN" : found_project;
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/shot")] = "UNKNOWN";
                        i.set_name("UNKNOWN");
                        i.set_flag("#FFFF0000");  // Red
                        i.set_enabled(false);
                    }

                    auto meta = i.prop();
                    meta.update(pm, true);
                    i.set_prop(meta);

                    if (!only_create_conform_track) {
                        anon_mail(timeline::item_prop_atom_v, meta).send(i.actor());
                        anon_mail(timeline::item_flag_atom_v, i.flag()).send(i.actor());
                        if (is_valid) {
                            anon_mail(timeline::item_name_atom_v, shot).send(i.actor());
                        } else {
                            anon_mail(timeline::item_name_atom_v, std::string("UNKNOWN")).send(i.actor());
                        }
                    }
                }
                tframe += i.trimmed_duration();
            }

            // Create new conform track if requested
            if (only_create_conform_track) {
                vtrack.reset_uuid(true);
                vtrack.reset_actor(true);
                vtrack.reset_media_uuid();
                auto vua = UuidActor(vtrack.uuid(), spawn<timeline::TrackActor>(vtrack));

                request_receive<JsonStore>(
                    *sys,
                    timeline_item.children().front().actor(),
                    timeline::insert_item_atom_v,
                    insert_index,
                    UuidActorVector({vua}));
            }

            // Store conform track UUID in timeline props
            auto tprop = timeline_item.prop();
            tprop["conform_track_uuid"] = vtrack.uuid();
            request_receive<JsonStore>(
                *sys, timeline_item.actor(), timeline::item_prop_atom_v, tprop);

            spdlog::info("RodeoConformActor: Timeline prepared successfully");
            rp.deliver(true);
        } catch (const std::exception &err) {
            spdlog::warn("RodeoConformActor: prepare_timeline error: {}", err.what());
            rp.deliver(false);
        }
    }

    /**
     * Find matching media by key.
     */
    void find_matching(
        caf::typed_response_promise<utility::UuidActorVector> rp,
        const std::string &key,
        const std::pair<utility::UuidActor, utility::JsonStore> &needle,
        const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &haystack) {

        auto result = utility::UuidActorVector();

        // Match by ShotGrid version_id
        auto needle_version_id = needle.second.value(shotgrid_version_id_ptr, -1);
        if (needle_version_id <= 0) {
            rp.deliver(result);
            return;
        }

        for (const auto &i : haystack) {
            if (i.first.uuid() == needle.first.uuid())
                continue;

            try {
                auto haystack_version_id = i.second.value(shotgrid_version_id_ptr, -1);
                if (haystack_version_id > 0 && haystack_version_id == needle_version_id)
                    result.push_back(i.first);
            } catch (...) {}
        }

        rp.deliver(result);
    }

  private:
    caf::behavior behavior_;
    T conform_;
    caf::actor data_source_;  // Cached data source actor
};

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<ConformPlugin<RodeoConformActor<RodeoConform>>>(
                Uuid("b8c9d0e1-f2a3-4b5c-8d7e-9f0a1b2c3d4e"),
                "RodeoFX",
                "xStudio",  // Use "xStudio" as author to auto-enable plugin
                "RodeoFX Smart Conformer",
                semver::version("2.0.0"))}));
}
}
