// SPDX-License-Identifier: Apache-2.0
/**
 * Rodeo Conform Plugin for xStudio
 *
 * Provides conforming functionality for RodeoFX timelines:
 * - Auto-conform from track names (e.g., "Comp: Latest", "Anim: Delivered")
 * - Match media to clips by show + shot code
 * - Prepare timeline with conform track structure
 *
 * Metadata paths used:
 *   /shotgrid/shot_code       - Shot code (e.g., "206044_0010")
 *   /shotgrid/project_code    - Project code (e.g., "drb")
 *   /shotgrid/version_id      - ShotGrid version ID
 *   /shotgrid/version_name    - Full version code
 *   /shotgrid/department      - Department (e.g., "Comp")
 *   /shotgrid/status          - Status (e.g., "dlvr")
 *
 * OCIO context (for colour pipeline):
 *   /colour_pipeline/ocio_context/show_code
 *   /colour_pipeline/ocio_context/shot_code
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
#include "xstudio/utility/json_store_sync.hpp"

using namespace xstudio;
using namespace xstudio::conform;
using namespace xstudio::utility;
using namespace std::chrono_literals;

namespace {

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
const auto external_seq_ptr =
    nlohmann::json::json_pointer("/metadata/external/RodeoFX/seq");
const auto external_shot_ptr =
    nlohmann::json::json_pointer("/metadata/external/RodeoFX/shot");

// Clip metadata (set by conform/browser plugins)
const auto clip_shot_ptr =
    nlohmann::json::json_pointer("/metadata/external/Rodeo/shot");
const auto clip_show_ptr =
    nlohmann::json::json_pointer("/metadata/external/Rodeo/show");

/**
 * Extract project/show code from metadata.
 *
 * Tries multiple paths in order:
 *   1. /shotgrid/project_code
 *   2. /metadata/external/RodeoFX/show
 *   3. /metadata/external/Rodeo/show
 *
 * @param metadata JSON metadata object
 * @return Project code string or empty string if not found
 */
std::string get_project_name(const JsonStore &metadata) {
    try {
        if (metadata.contains(shotgrid_project_code_ptr) &&
            !metadata.at(shotgrid_project_code_ptr).is_null()) {
            return metadata.at(shotgrid_project_code_ptr).get<std::string>();
        }
    } catch (...) {
    }

    try {
        if (metadata.contains(external_show_ptr) &&
            !metadata.at(external_show_ptr).is_null()) {
            return metadata.at(external_show_ptr).get<std::string>();
        }
    } catch (...) {
    }

    try {
        if (metadata.contains(clip_show_ptr) && !metadata.at(clip_show_ptr).is_null()) {
            return metadata.at(clip_show_ptr).get<std::string>();
        }
    } catch (...) {
    }

    return "";
}

/**
 * Extract shot code from metadata.
 *
 * Tries multiple paths in order:
 *   1. /shotgrid/shot_code
 *   2. /metadata/external/RodeoFX/shot
 *   3. /metadata/external/Rodeo/shot
 *
 * @param metadata JSON metadata object
 * @return Shot code string or empty string if not found
 */
std::string get_shot_name(const JsonStore &metadata) {
    try {
        if (metadata.contains(shotgrid_shot_code_ptr) &&
            !metadata.at(shotgrid_shot_code_ptr).is_null()) {
            return metadata.at(shotgrid_shot_code_ptr).get<std::string>();
        }
    } catch (...) {
    }

    try {
        if (metadata.contains(external_shot_ptr) &&
            !metadata.at(external_shot_ptr).is_null()) {
            return metadata.at(external_shot_ptr).get<std::string>();
        }
    } catch (...) {
    }

    try {
        if (metadata.contains(clip_shot_ptr) && !metadata.at(clip_shot_ptr).is_null()) {
            return metadata.at(clip_shot_ptr).get<std::string>();
        }
    } catch (...) {
    }

    return "";
}

/**
 * Extract show code from timeline name.
 *
 * Expected format: show_sequence_... (e.g., "drb_206044_cut_context_Comp")
 *
 * @param timeline_name Timeline name string
 * @return Show code (first segment before underscore) or empty string
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
 * Extract project from path using RodeoFX patterns.
 *
 * Patterns:
 *   /rdo/shows/{show}/...
 *   /shows/{show}/...
 *
 * @param path POSIX path
 * @return Show code or empty string
 */
std::string extract_project_from_path(const std::string &path) {
    static const std::regex show_regex(R"(/(?:rdo/)?shows/([^/]+)/)");

    std::smatch match;
    if (std::regex_search(path, match, show_regex)) {
        std::string show = match[1].str();
        std::transform(show.begin(), show.end(), show.begin(), ::tolower);
        return show;
    }
    return "";
}

} // anonymous namespace

/**
 * RodeoConform - Main conformer class for Rodeo timelines.
 *
 * Extends the base Conformer class to provide Rodeo-specific
 * conform operations including:
 *   - Reading conform tasks from /core/conform/presets
 *   - Matching media to clips by show + shot code
 *   - Preparing timeline conform tracks
 */
class RodeoConform : public Conformer {
  public:
    RodeoConform(const utility::JsonStore &prefs = utility::JsonStore()) : Conformer(prefs) {}
    ~RodeoConform() = default;

    void update_preferences(const utility::JsonStore &prefs) override {
        try {
            purge_sequence_on_import_ = global_store::preference_value<bool>(
                prefs, "/plugin/conformer/rodeo/purge_sequence_on_import");
            reuse_media_ = global_store::preference_value<bool>(
                prefs, "/plugin/conformer/rodeo/reuse_media");
        } catch (const std::exception &err) {
            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
        }
    }

    /**
     * Return list of visible conform tasks.
     *
     * Tasks are read from /core/conform/presets and filtered
     * to only include favourited, visible items.
     */
    std::vector<std::string> conform_tasks() override {
        if (visible_tasks_.empty())
            spdlog::warn("conform_tasks() empty ? {}", __PRETTY_FUNCTION__);
        return visible_tasks_;
    }

    /**
     * Update tasks from presets configuration.
     *
     * Parses the presets JSON structure to extract task names.
     * The presets format is:
     *   { "children": [
     *       { "name": "...", "hidden": false, "favourite": true,
     *         "flags": ["Replace", "Conform", ...],
     *         "children": [null, {"children": [{"name": "task_name", ...}]}]
     *       }
     *     ]
     *   }
     *
     * @param presets JsonStoreSync containing preset data
     * @return true if tasks changed, false otherwise
     */
    bool update_tasks(const utility::JsonStoreSync &presets) {
        auto result = false;
        std::vector<std::string> tasks;
        std::vector<std::string> visible_tasks;
        std::map<std::string, utility::Uuid> task_uuids;

        try {
            for (const auto &i : presets.as_json().at("children")) {
                if (i.value("hidden", false) or not i.value("favourite", false))
                    continue;

                auto flags = i.value("flags", std::set<std::string>());

                if (flags.count("Replace") or flags.count("Conform") or
                    flags.count("Compare")) {
                    // scan children for tasks
                    for (const auto &j : i.at("children").at(1).at("children")) {
                        if (j.value("hidden", false))
                            continue;

                        task_uuids[j.value("name", "")] = j.value("id", utility::Uuid());
                        tasks.emplace_back(j.value("name", ""));

                        if (not j.value("favourite", true))
                            continue;

                        visible_tasks.emplace_back(j.value("name", ""));
                    }
                }
            }
        } catch (const std::exception &err) {
            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
        }

        if (tasks != tasks_ or visible_tasks_ != visible_tasks) {
            visible_tasks_ = visible_tasks;
            tasks_         = tasks;
            task_uuids_    = task_uuids;
            result         = true;
        }

        return result;
    }

    std::optional<utility::Uuid> get_task_id(const std::string &name) const {
        if (task_uuids_.count(name))
            return task_uuids_.at(name);
        return {};
    }

    [[nodiscard]] bool purge_sequence_on_import() const { return purge_sequence_on_import_; }
    [[nodiscard]] bool reuse_media() const { return reuse_media_; }

  private:
    std::vector<std::string> tasks_;
    std::vector<std::string> visible_tasks_;
    std::map<std::string, utility::Uuid> task_uuids_;
    bool purge_sequence_on_import_{false};
    bool reuse_media_{true};
};

/**
 * RodeoConformActor - CAF actor wrapping RodeoConform.
 *
 * Handles async message passing for conform operations:
 *   - conform_atom + ConformRequest -> ConformReply (match media to clips)
 *   - conform_atom + task + ConformRequest -> ConformReply (task-based conform)
 *   - conform_atom + timeline + bool -> bool (prepare timeline)
 *   - conform_tasks_atom -> vector<string> (list available tasks)
 */
template <typename T>
class RodeoConformActor : public caf::event_based_actor {
  public:
    RodeoConformActor(
        caf::actor_config &cfg, const utility::JsonStore &prefs = utility::JsonStore())
        : caf::event_based_actor(cfg), conform_(prefs) {
        spdlog::debug("Created RodeoConformActor");
        utility::print_on_exit(this, "RodeoConformActor");

        {
            auto prefs = global_store::GlobalStoreHelper(system());
            utility::JsonStore js;
            utility::join_broadcast(this, prefs.get_group(js));
            conform_.update_preferences(js);
        }

        behavior_.assign(
            [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {},

            // Conform media into clips - keys off show/shot
            [=](conform_atom, const ConformRequest &crequest) -> result<ConformReply> {
                auto rp = make_response_promise<ConformReply>();
                conform_request(rp, crequest);
                return rp;
            },

            // Task-based conform (e.g., "Comp: Latest")
            [=](conform_atom,
                const std::string &conform_task,
                const ConformRequest &crequest) -> result<ConformReply> {
                auto rp = make_response_promise<ConformReply>();
                conform_task_request(rp, conform_task, crequest);
                return rp;
            },

            // Prepare timeline for conform (create conform track, extract shot structure)
            [=](conform_atom,
                const UuidActor &timeline,
                const bool only_create_conform_track) -> result<bool> {
                auto rp = make_response_promise<bool>();
                prepare_timeline(rp, timeline, only_create_conform_track);
                return rp;
            },

            // Find timelines/sequences related to media
            [=](conform_atom,
                const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &media)
                -> result<std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>> {
                auto rp = make_response_promise<std::vector<
                    std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>>();
                conform_media(rp, media);
                return rp;
            },

            // Find matching media by key (e.g., version_id)
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
                setup();
                conform_.update_tasks(presets_);
                return conform_.conform_tasks();
            },

            // Handle preset updates from RDO Browser
            [=](utility::event_atom,
                json_store::sync_atom,
                const Uuid &uuid,
                const JsonStore &event) {
                if (uuid == user_preset_event_id_) {
                    presets_.process_event(event);
                    if (conform_.update_tasks(presets_))
                        anon_mail(conform_tasks_atom_v)
                            .send(
                                system().registry().template get<caf::actor>(conform_registry));
                }
            },

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
     *
     * Looks up the task in presets to get query parameters,
     * then queries RDO Browser data source for matching versions.
     */
    void conform_task_request(
        caf::typed_response_promise<ConformReply> rp,
        const std::string &conform_task,
        const ConformRequest &crequest) {

        try {
            if (not connected_) {
                setup();
                conform_.update_tasks(presets_);
            }

            if (crequest.items_.empty()) {
                rp.deliver(ConformReply(crequest));
            } else {
                auto query_id = conform_.get_task_id(conform_task);

                if (query_id) {
                    // Find the RDO Browser data source actor
                    auto rdo_browser =
                        system().registry().template get<caf::actor>("RDO_BROWSER");

                    if (not rdo_browser) {
                        // Try fallback registry name
                        rdo_browser =
                            system().registry().template get<caf::actor>("SHOTBROWSER");
                    }

                    if (not rdo_browser) {
                        spdlog::warn("{} Failed to find RDO Browser", __PRETTY_FUNCTION__);
                        rp.deliver(ConformReply(crequest));
                        return;
                    }

                    auto shotgrid_count = std::make_shared<size_t>(crequest.items_.size());
                    auto shotgrid_results =
                        std::make_shared<std::vector<UuidActorVector>>(crequest.items_.size());

                    const auto limit_results =
                        crequest.operations_.value("limit_to_one_result", false);
                    const auto reuse_media =
                        crequest.operations_.value("reuse_media", false) &&
                        conform_.reuse_media();

                    // Build lookup cache from reuse structure
                    auto reuse_map = std::multimap<
                        int,
                        std::pair<UuidActor, std::pair<std::string, std::string>>>();

                    if (reuse_media) {
                        for (const auto &m : crequest.reuse_list_) {
                            try {
                                auto version_id =
                                    m.first.second.value(shotgrid_version_id_ptr, -1);
                                if (version_id > 0) {
                                    reuse_map.insert(std::make_pair(
                                        version_id,
                                        std::make_pair(m.first.first, m.second)));
                                }
                            } catch (...) {
                            }
                        }
                    }

                    // Dispatch requests for ShotGrid data
                    for (size_t i = 0; i < crequest.items_.size(); i++) {
                        auto metadata = crequest.metadata_.at(crequest.items_.at(i).item_.uuid());

                        // Clip metadata has precedence over media metadata
                        auto media_uuid = metadata.value("media_uuid", Uuid());
                        if (not media_uuid.is_null() && crequest.metadata_.count(media_uuid)) {
                            auto tmp = crequest.metadata_.at(media_uuid);
                            tmp.update(metadata, true);
                            metadata = tmp;
                        }

                        // Build query request
                        auto req          = JsonStore(R"({
                            "action": "execute_preset",
                            "project_code": null,
                            "preset_id": null,
                            "metadata": null,
                            "context": {
                                "type": null,
                                "epoc": null,
                                "audio_source": [],
                                "sequence_source": [],
                                "visual_source": [],
                                "flag_text": "",
                                "flag_colour": "",
                                "truncated": false
                            }
                        })"_json);

                        auto project = get_project_name(metadata);
                        req["project_code"] = project;
                        req["preset_id"]    = *query_id;
                        req["metadata"]     = metadata;

                        mail(data_source::get_data_atom_v, req)
                            .request(rdo_browser, infinite)
                            .then(
                                [=](JsonStore result) mutable {
                                    auto final_media = UuidActorVector();

                                    if (limit_results) {
                                        try {
                                            if (result.at("result").at("data").size() > 1)
                                                result["result"]["data"].erase(
                                                    std::next(
                                                        result["result"]["data"].begin(), 1),
                                                    result["result"]["data"].end());
                                        } catch (...) {
                                        }
                                    }

                                    if (reuse_media) {
                                        try {
                                            auto it = result.at("result").at("data").begin();
                                            while (it != result.at("result").at("data").end()) {
                                                auto version_id = it->at("id").get<int>();
                                                auto match      = reuse_map.equal_range(version_id);
                                                auto matched    = false;

                                                for (auto m = match.first; m != match.second;
                                                     ++m) {
                                                    final_media.push_back(m->second.first);
                                                    matched = true;
                                                    break;
                                                }
                                                if (matched)
                                                    it = result.at("result")
                                                             .at("data")
                                                             .erase(it);
                                                else
                                                    it++;
                                            }
                                        } catch (...) {
                                        }
                                    }

                                    // Add new media from query results
                                    mail(
                                        playlist::add_media_atom_v,
                                        result,
                                        crequest.container_.uuid(),
                                        crequest.container_.actor(),
                                        crequest.items_.at(i).before_)
                                        .request(rdo_browser, infinite)
                                        .then(
                                            [=](const UuidActorVector &new_media) mutable {
                                                final_media.insert(
                                                    final_media.end(),
                                                    new_media.begin(),
                                                    new_media.end());

                                                (*shotgrid_results)[i] = final_media;
                                                (*shotgrid_count)--;
                                                if (not *shotgrid_count)
                                                    process_results(
                                                        rp, *shotgrid_results, crequest);
                                            },
                                            [=](caf::error &err) mutable {
                                                spdlog::warn(
                                                    "{} add_media failed: {}",
                                                    __PRETTY_FUNCTION__,
                                                    to_string(err));
                                                (*shotgrid_count)--;
                                                if (not *shotgrid_count)
                                                    process_results(
                                                        rp, *shotgrid_results, crequest);
                                            });
                                },
                                [=](caf::error &err) mutable {
                                    spdlog::warn(
                                        "{} query failed: {}",
                                        __PRETTY_FUNCTION__,
                                        to_string(err));
                                    (*shotgrid_count)--;
                                    if (not *shotgrid_count)
                                        process_results(rp, *shotgrid_results, crequest);
                                });
                    }
                } else {
                    spdlog::warn(
                        "{} Failed to find query id for task '{}'",
                        __PRETTY_FUNCTION__,
                        conform_task);
                    rp.deliver(ConformReply(crequest));
                }
            }
        } catch (const std::exception &err) {
            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

    /**
     * Handle basic conform request - match media to clips by show/shot.
     *
     * For each item in the request, looks up its project and shot from metadata,
     * then finds matching clips in the template tracks.
     */
    void conform_request(
        caf::typed_response_promise<ConformReply> rp, const ConformRequest &crequest) {
        try {
            auto creply = ConformReply(crequest);
            auto clips  = crequest.template_tracks_.at(0).find_all_items(timeline::IT_CLIP);

            // Build clip lookup maps
            std::map<Uuid, std::string> clip_project_map;
            std::map<Uuid, std::string> clip_shot_map;

            for (const auto &c : clips) {
                auto clip_uuid  = c.get().uuid();
                auto media_uuid = c.get().prop().value("media_uuid", Uuid());

                auto clip_project = get_project_name(crequest.metadata_.at(clip_uuid));
                auto clip_shot    = get_shot_name(crequest.metadata_.at(clip_uuid));

                // Fallback to media metadata if clip metadata is empty
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

                if (clip_project.empty() || clip_shot.empty()) {
                    spdlog::warn(
                        "Clip metadata not found: {} project='{}' shot='{}'",
                        c.get().name(),
                        clip_project,
                        clip_shot);
                }
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
                    spdlog::warn("No clips found on selected conform track.");
                    creply.items_.push_back({});
                } else {
                    try {
                        const auto meta = crequest.metadata_.at(i.item_.uuid());
                        auto project    = get_project_name(meta);
                        auto shot       = get_shot_name(meta);

                        if (project.empty() || shot.empty()) {
                            creply.items_.push_back({});
                            spdlog::warn(
                                "Media missing metadata: {} project='{}' shot='{}'",
                                to_string(i.item_.uuid()),
                                project,
                                shot);
                        } else {
                            auto ritems = std::vector<ConformReplyItem>();

                            for (const auto &c : clips) {
                                auto clip_uuid = c.get().uuid();
                                if ((!only_one_match || !matched_clips.count(clip_uuid)) &&
                                    clip_project_map.at(clip_uuid) == project &&
                                    clip_shot_map.at(clip_uuid) == shot) {
                                    ritems.push_back(std::make_tuple(c.get().uuid_actor()));
                                    matched_clips.insert(clip_uuid);
                                    if (only_one_match)
                                        break;
                                }
                            }

                            if (ritems.empty()) {
                                spdlog::warn(
                                    "Media has no matching clip: {} project='{}' shot='{}'",
                                    to_string(i.item_.uuid()),
                                    project,
                                    shot);
                                creply.items_.push_back({});
                            } else {
                                creply.items_.push_back(ritems);
                            }
                        }
                    } catch (const std::exception &err) {
                        spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
                        creply.items_.push_back({});
                    }
                }
            }

            rp.deliver(creply);
        } catch (const std::exception &err) {
            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

    /**
     * Prepare timeline for conform operations.
     *
     * Creates a conform track based on the bottom video track,
     * populates it with shot metadata, and optionally purges empty tracks.
     */
    void prepare_timeline(
        caf::typed_response_promise<bool> rp,
        const UuidActor &timeline,
        const bool only_create_conform_track) {

        // Regex for extracting project from RodeoFX paths
        static const auto SHOW_REGEX =
            std::regex(R"(^(?:/rdo)?/shows/([^/]+)/.+$)");

        scoped_actor sys{system()};
        try {
            auto found_project = std::string();
            auto timeline_item =
                request_receive<timeline::Item>(*sys, timeline.actor(), timeline::item_atom_v);

            // Try to extract project from timeline path
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

            // Fallback: extract project from timeline name
            if (found_project.empty()) {
                found_project = extract_show_from_timeline_name(timeline_item.name());
            }

            // Process timeline - purge empty tracks if requested
            auto video_tracks = timeline_item.find_all_items(timeline::IT_VIDEO_TRACK);
            auto insert_index = static_cast<int>(video_tracks.size());
            auto vcount       = video_tracks.size();

            if (!only_create_conform_track) {
                for (auto &i : video_tracks) {
                    if (i.get().empty() && vcount > 1) {
                        auto pactor = find_parent_actor(timeline_item, i.get().uuid());
                        if (pactor) {
                            insert_index--;
                            vcount--;
                            request_receive<JsonStore>(
                                *sys,
                                pactor,
                                timeline::erase_item_atom_v,
                                i.get().uuid(),
                                true);
                        }
                    }
                }

                auto audio_tracks = timeline_item.find_all_items(timeline::IT_AUDIO_TRACK);
                auto acount       = audio_tracks.size();
                for (auto &i : audio_tracks) {
                    if (i.get().empty() && acount > 1) {
                        auto pactor = find_parent_actor(timeline_item, i.get().uuid());
                        if (pactor) {
                            acount--;
                            request_receive<JsonStore>(
                                *sys,
                                pactor,
                                timeline::erase_item_atom_v,
                                i.get().uuid(),
                                true);
                        }
                    }
                }
            }

            // Create conform track from bottom video track
            auto vtrack = timeline::Item(timeline::IT_NONE);
            std::reverse(video_tracks.begin(), video_tracks.end());

            for (const auto &i : video_tracks) {
                if (!i.get().empty()) {
                    vtrack = i.get();
                    break;
                }
            }

            vtrack.set_name("Conform Track");
            vtrack.set_locked(true);
            vtrack.set_enabled(false);

            if (!only_create_conform_track)
                anon_mail(timeline::item_lock_atom_v, true).send(vtrack.actor());

            auto media_metadata = std::map<Uuid, JsonStore>();
            auto tframe         = timeline_item.trimmed_start();
            const auto trate    = timeline_item.rate();

            auto unknown_name = std::string("UNKNOWN");

            // Process each clip on the conform track
            for (auto &i : vtrack.children()) {
                if (i.item_type() == timeline::IT_CLIP) {
                    auto is_valid = false;
                    auto project  = R"(null)"_json;
                    auto shot     = R"(null)"_json;

                    auto pm =
                        R"({"metadata": {"external": {"Rodeo": {"shot": null, "show": null}}}})"_json;

                    // Try to get metadata from underlying media
                    if (!is_valid) {
                        auto items = timeline_item.resolve_time_raw(tframe);

                        for (const auto &j : items) {
                            auto clip = j.first;

                            try {
                                project = get_project_name(clip.prop());
                                shot    = get_shot_name(clip.prop());

                                if (project.get<std::string>().empty() ||
                                    shot.get<std::string>().empty()) {
                                    // Try media metadata
                                    auto media_uuid = clip.prop().value("media_uuid", Uuid());

                                    if (!media_uuid.is_null() &&
                                        !media_metadata.count(media_uuid)) {
                                        try {
                                            auto tries    = 10;
                                            auto metadata = JsonStore();
                                            while (true) {
                                                metadata = request_receive<JsonStore>(
                                                    *sys,
                                                    clip.actor(),
                                                    playlist::get_media_atom_v,
                                                    json_store::get_json_atom_v,
                                                    Uuid(),
                                                    "");
                                                tries--;
                                                if (!tries || !metadata.is_null())
                                                    break;
                                                std::this_thread::sleep_for(200ms);
                                            }

                                            if (metadata.is_null())
                                                spdlog::warn(
                                                    "No metadata {} {} {}",
                                                    clip.name(),
                                                    metadata.dump(2),
                                                    tries);

                                            media_metadata[media_uuid] = metadata;
                                        } catch (const std::exception &err) {
                                            spdlog::warn(
                                                "{} {} {}",
                                                __PRETTY_FUNCTION__,
                                                clip.name(),
                                                err.what());
                                            media_metadata[media_uuid] = R"({})"_json;
                                        }
                                    }

                                    if (!media_uuid.is_null() &&
                                        media_metadata.count(media_uuid)) {
                                        if (project.get<std::string>().empty())
                                            project = get_project_name(
                                                media_metadata.at(media_uuid));
                                        if (shot.get<std::string>().empty())
                                            shot = get_shot_name(media_metadata.at(media_uuid));
                                    }
                                }

                                if (!project.get<std::string>().empty())
                                    found_project = project;

                                if (!project.get<std::string>().empty() &&
                                    !shot.get<std::string>().empty()) {
                                    is_valid = true;
                                    break;
                                }
                            } catch (const std::exception &err) {
                                spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
                            }
                        }
                    }

                    // Validate shot name (alphanumeric and underscore only)
                    const static std::regex valid_shot_re(R"(^[a-zA-Z0-9_]+$)");
                    if (!shot.is_null() &&
                        !std::regex_match(shot.get<std::string>(), valid_shot_re))
                        is_valid = false;

                    // Update clip metadata
                    if (!is_valid) {
                        auto meta = i.prop();
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/shot")] =
                            unknown_name;
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/show")] =
                            found_project.empty() ? unknown_name : found_project;

                        meta.update(pm, true);

                        if (!only_create_conform_track) {
                            anon_mail(timeline::item_prop_atom_v, meta).send(i.actor());
                            anon_mail(timeline::item_name_atom_v, unknown_name).send(i.actor());
                            anon_mail(timeline::item_flag_atom_v, "#FFFF0000").send(i.actor());
                        }

                        i.set_prop(meta);
                        i.set_name(unknown_name);
                        i.set_flag("#FFFF0000");
                        i.set_enabled(false);
                    } else {
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/show")] =
                            project;
                        pm[nlohmann::json::json_pointer("/metadata/external/Rodeo/shot")] =
                            shot;

                        if (found_project.empty())
                            found_project = project;

                        auto meta = i.prop();
                        meta.update(pm, true);

                        i.set_prop(meta);
                        i.set_name(shot);
                        i.set_flag("#FF00FF00");
                        i.set_enabled(true);

                        if (!only_create_conform_track) {
                            anon_mail(timeline::item_prop_atom_v, meta).send(i.actor());
                            anon_mail(timeline::item_name_atom_v, shot.get<std::string>())
                                .send(i.actor());
                            anon_mail(timeline::item_flag_atom_v, "#FF00FF00").send(i.actor());
                        }
                    }
                }
                tframe += i.trimmed_duration();
            }

            if (only_create_conform_track) {
                // Clean before adding
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

            auto tprop                  = timeline_item.prop();
            tprop["conform_track_uuid"] = vtrack.uuid();
            request_receive<JsonStore>(
                *sys, timeline_item.actor(), timeline::item_prop_atom_v, tprop);

            // Purge other video tracks if configured and importing turnover
            if (!only_create_conform_track && conform_.purge_sequence_on_import() &&
                vcount > 1) {
                // Only purge if this looks like a turnover import
                auto tmeta = request_receive<JsonStore>(
                    *sys,
                    timeline.actor(),
                    json_store::get_json_atom_v,
                    "/metadata/shotgrid/version/attributes");
                if (tmeta.value("sg_twig_type", "") == "data/clip/cut" &&
                    ends_with(tmeta.value("sg_twig_name", ""), "_turnover"))
                    request_receive<JsonStore>(
                        *sys,
                        timeline_item.children().front().actor(),
                        timeline::erase_item_atom_v,
                        0,
                        static_cast<int>(vcount - 1),
                        true);
            }

            // Wait for timeline to sync
            if (!only_create_conform_track) {
                auto done = false;
                while (!done) {
                    try {
                        auto conform_item = request_receive<timeline::Item>(
                            *sys, timeline.actor(), timeline::item_atom_v, vtrack.uuid());
                        done = true;
                        for (const auto &i : conform_item) {
                            if (i.item_type() == timeline::IT_CLIP &&
                                !(i.flag() == "#FFFF0000" || i.flag() == "#FF00FF00")) {
                                done = false;
                                spdlog::info(
                                    "Waiting for timeline to sync. {}", timeline_path);
                                std::this_thread::sleep_for(1s);
                                break;
                            }
                        }
                    } catch (...) {
                        spdlog::warn("can't find conform track");
                        break;
                    }
                }
                spdlog::info("Timeline is ready. {}", timeline_path);
            }

            rp.deliver(true);
        } catch (const std::exception &err) {
            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
            rp.deliver(false);
        }
    }

    /**
     * Find sequences/timelines related to given media.
     *
     * Currently a placeholder - returns empty results.
     * Can be extended to query ShotGrid for sequence versions.
     */
    void conform_media(
        caf::typed_response_promise<std::vector<
            std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>> rp,
        const std::vector<std::pair<utility::UuidActor, utility::JsonStore>> &media) {

        // Return empty results - sequence lookup not implemented yet
        rp.deliver(std::vector<
                   std::optional<std::tuple<std::string, caf::uri, utility::JsonStore>>>(
            media.size()));
    }

    /**
     * Setup connection to RDO Browser data source.
     */
    void setup() {
        if (!connected_) {
            // Try to find RDO Browser data source
            browser_actor_ = system().registry().template get<caf::actor>("RDO_BROWSER");
            if (!browser_actor_) {
                browser_actor_ = system().registry().template get<caf::actor>("SHOTBROWSER");
            }

            if (browser_actor_) {
                scoped_actor sys{system()};
                try {
                    auto uuids = request_receive<UuidVector>(
                        *sys, browser_actor_, json_store::sync_atom_v);
                    if (!uuids.empty()) {
                        user_preset_event_id_ = uuids[0];

                        // Get presets
                        auto data = request_receive<JsonStore>(
                            *sys, browser_actor_, json_store::sync_atom_v, user_preset_event_id_);
                        presets_ = JsonStoreSync(data);

                        // Join events
                        if (preset_events_) {
                            try {
                                request_receive<bool>(
                                    *sys,
                                    preset_events_,
                                    broadcast::leave_broadcast_atom_v,
                                    this);
                            } catch (...) {
                            }
                            preset_events_ = caf::actor();
                        }

                        try {
                            preset_events_ = request_receive<caf::actor>(
                                *sys, browser_actor_, get_event_group_atom_v);
                            request_receive<bool>(
                                *sys,
                                preset_events_,
                                broadcast::join_broadcast_atom_v,
                                this);
                        } catch (const std::exception &err) {
                            spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
                        }

                        connected_ = true;
                    }
                } catch (const std::exception &err) {
                    spdlog::warn("{} {}", __PRETTY_FUNCTION__, err.what());
                }
            } else {
                spdlog::debug("{} Browser not found (this is normal if not loaded yet)",
                             __PRETTY_FUNCTION__);
            }
        }
    }

    /**
     * Process query results into ConformReply.
     */
    void process_results(
        caf::typed_response_promise<ConformReply> rp,
        const std::vector<UuidActorVector> &results,
        const ConformRequest &crequest) {
        auto creply = ConformReply(crequest);

        for (const auto &i : results) {
            auto ritems = std::vector<ConformReplyItem>();

            for (const auto &j : i)
                ritems.emplace_back(std::make_tuple(j));

            creply.items_.push_back(ritems);
        }

        creply.operations_["create_media"] = true;
        creply.operations_["insert_media"] = true;

        rp.deliver(creply);
    }

    /**
     * Find matching media by key (e.g., version_id).
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
            } catch (...) {
            }
        }

        rp.deliver(result);
    }

  private:
    caf::behavior behavior_;
    T conform_;
    utility::Uuid user_preset_event_id_;
    utility::JsonStoreSync presets_;
    caf::actor browser_actor_;
    caf::actor preset_events_;
    bool connected_{false};
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
