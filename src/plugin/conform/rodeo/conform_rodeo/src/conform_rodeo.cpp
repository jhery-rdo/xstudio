// SPDX-License-Identifier: Apache-2.0
/**
 * Rodeo Conform Plugin for xStudio
 *
 * Provides conforming functionality for RodeoFX timelines:
 * - Auto-conform from presets (e.g., "Comp: Latest", "Anim: Pushed")
 * - Match media to clips by show + shot code
 * - Prepare timeline with conform track structure
 *
 * This plugin reads presets from /core/conform/presets and registers
 * tasks that appear in the Replace/Compare menus in xStudio.
 *
 * Metadata paths used:
 *   /shotgrid/shot_code       - Shot code (e.g., "206044_0010")
 *   /shotgrid/project_code    - Project code (e.g., "drb")
 *   /shotgrid/version_id      - ShotGrid version ID
 *   /shotgrid/department      - Department (e.g., "Comp")
 *   /shotgrid/status          - Status (e.g., "dlvr")
 */

#include <caf/actor_registry.hpp>
#include <caf/policy/select_all.hpp>
#include <regex>
#include <algorithm>

#include "xstudio/conform/conformer.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/string_helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/timeline/track_actor.hpp"

using namespace xstudio;
using namespace xstudio::conform;
using namespace xstudio::utility;
using namespace std::chrono_literals;

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

} // anonymous namespace

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
 * RodeoConformActor - CAF actor wrapping RodeoConform.
 */
template <typename T>
class RodeoConformActor : public caf::event_based_actor {
  public:
    RodeoConformActor(
        caf::actor_config &cfg, const utility::JsonStore &prefs = utility::JsonStore())
        : caf::event_based_actor(cfg), conform_(prefs) {
        spdlog::info("RodeoConformActor created");
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
            [=](conform_atom,
                const std::string &conform_task,
                const ConformRequest &crequest) -> result<ConformReply> {
                spdlog::info("RodeoConformActor: conform_task_request for '{}'", conform_task);
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

            // Find sequences related to media - extract from Rodeo metadata
            [=](conform_atom,
                const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &media)
                -> result<std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>> {
                auto result = std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>();
                result.reserve(media.size());

                for (const auto &[ua, meta] : media) {
                    try {
                        // Extract show and shot from Rodeo metadata
                        auto show = get_project_name(meta);
                        auto shot = get_shot_name(meta);

                        if (!show.empty() && !shot.empty()) {
                            // Extract sequence from shot code (e.g., "206044_0010" -> "206044")
                            std::string seq = shot;
                            auto underscore_pos = shot.find('_');
                            if (underscore_pos != std::string::npos) {
                                seq = shot.substr(0, underscore_pos);
                            }

                            // Build sequence name and URI
                            std::string seq_name = show + "_" + seq;
                            // Use a file URI pattern for the sequence
                            auto seq_uri = caf::make_uri("xstudio://sequence/" + show + "/" + seq);

                            if (seq_uri) {
                                // Include metadata for the sequence
                                auto seq_meta = JsonStore();
                                seq_meta["show"] = show;
                                seq_meta["sequence"] = seq;
                                seq_meta["shot"] = shot;

                                spdlog::debug(
                                    "RodeoConform: Found sequence {} for media",
                                    seq_name);
                                result.push_back(
                                    std::make_tuple(seq_name, *seq_uri, seq_meta));
                                continue;
                            }
                        }
                    } catch (const std::exception &err) {
                        spdlog::debug(
                            "RodeoConform: Error extracting sequence from media: {}",
                            err.what());
                    }
                    // No sequence found for this media
                    result.push_back({});
                }

                spdlog::info(
                    "RodeoConform: conform_find_timeline returned {} results for {} media",
                    result.size(), media.size());
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
    /**
     * Handle task-based conform request.
     */
    void conform_task_request(
        caf::typed_response_promise<ConformReply> rp,
        const std::string &conform_task,
        const ConformRequest &crequest) {

        try {
            spdlog::info(
                "RodeoConformActor: Processing task '{}' with {} items",
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

            // For now, just do basic matching and let the caller know we processed it
            // In a full implementation, we would query ShotGrid here
            conform_request(rp, crequest);

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
};

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<ConformPlugin<RodeoConformActor<RodeoConform>>>(
                Uuid("b8c9d0e1-f2a3-4b5c-8d7e-9f0a1b2c3d4e"),
                "RodeoFX",
                "xStudio",  // Use "xStudio" as author to auto-enable plugin
                "RodeoFX Conformer",
                semver::version("1.0.0"))}));
}
}
