// SPDX-License-Identifier: Apache-2.0
#include <filesystem>
#include <regex>
#include <algorithm>
#include <set>
#include <cstdlib>

#include <spdlog/spdlog.h>
#include <OpenColorIO/OpenColorIO.h>

#include "xstudio/media_hook/media_hook.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/string_helpers.hpp"
#include "xstudio/utility/json_store.hpp"

namespace fs   = std::filesystem;
namespace OCIO = OCIO_NAMESPACE;

using namespace xstudio;
using namespace xstudio::media_hook;
using namespace xstudio::utility;

/**
 * RodeoFX Media Hook Plugin
 *
 * Automatically sets OCIO context variables (SEQ, SHOT, etc.) on media during loading.
 * This ensures the OCIO colour pipeline has the correct context before initializing.
 *
 * Also handles slate frame trimming for MOVs in /movhistory or .published folders.
 */
class RodeoMediaHook : public MediaHook {
  public:
    RodeoMediaHook() : MediaHook("RodeoFX") {

        auto_trim_slate_ = add_boolean_attribute("Auto Trim Slate", "Auto Trim Slate", true);
        auto_trim_slate_->set_preference_path("/plugin/rodeo_media_hook/auto_trim_slate");
        auto_trim_slate_->set_tool_tip(
            "Automatically trim the first frame (slate) from MOVs in /movhistory or "
            ".published folders.");

        rez_ocio_config_root_ = add_string_attribute(
            "REZ OCIO Config Root",
            "REZ OCIO Config Root",
            "/rdo/software/rez/packages/rdo_ocio_config/2.5.0");
        rez_ocio_config_root_->set_preference_path(
            "/plugin/rodeo_media_hook/rez_ocio_config_root");
        rez_ocio_config_root_->set_tool_tip(
            "Path to the REZ OCIO config package containing LUTs and transforms.");

        default_show_ =
            add_string_attribute("Default Show Code", "Default Show Code", "cdl");
        default_show_->set_preference_path("/plugin/rodeo_media_hook/default_show");
        default_show_->set_tool_tip(
            "Default show code to use when the show cannot be determined from the media path.");
    }

    ~RodeoMediaHook() override = default;

    /**
     * Modify media reference to trim slate frames from published MOVs.
     */
    std::optional<utility::MediaReference> modify_media_reference(
        const utility::MediaReference &mr, const utility::JsonStore &jsn) override {

        utility::MediaReference result = mr;
        bool changed                   = false;

        if (auto_trim_slate_->value() && mr.container()) {
            auto path = uri_to_posix_path(mr.uri());

            if (should_trim_slate(path)) {
                auto fr = result.frame_list();

                spdlog::debug(
                    "RodeoMediaHook::modify_media_reference BEFORE trim: path={} "
                    "frame_start={} frame_count={} timecode={} start_frame_offset={}",
                    path,
                    fr.start(),
                    fr.count(),
                    result.timecode().total_frames(),
                    result.start_frame_offset());

                // Skip trimming if already trimmed (start_frame_offset != 0)
                if (result.start_frame_offset() != 0) {
                    spdlog::debug(
                        "RodeoMediaHook::modify_media_reference SKIP trim: already trimmed "
                        "(start_frame_offset={})",
                        result.start_frame_offset());
                } else if (fr.pop_front()) {
                    result.set_frame_list(fr);
                    result.set_timecode(result.timecode() + 1);
                    result.set_start_frame_offset(result.start_frame_offset() - 1);
                    changed = true;

                    spdlog::debug(
                        "RodeoMediaHook::modify_media_reference AFTER trim: path={} "
                        "frame_start={} frame_count={} timecode={} start_frame_offset={}",
                        path,
                        fr.start(),
                        fr.count(),
                        result.timecode().total_frames(),
                        result.start_frame_offset());
                }
            }
        }

        if (!changed)
            return {};

        return result;
    }

    /**
     * Modify metadata to inject OCIO context variables for colour pipeline.
     */
    utility::JsonStore modify_metadata(
        const utility::MediaReference &mr, const utility::JsonStore &metadata) override {

        utility::JsonStore result = metadata;

        const caf::uri &uri =
            mr.container() || mr.uris().empty() ? mr.uri() : mr.uris()[0].first;

        const std::string path = to_string(uri);
        auto ppath             = uri_to_posix_path(uri);

        spdlog::debug("RodeoMediaHook::modify_metadata CALLED for: {}", ppath);

        // Build colour pipeline parameters
        auto colour_p             = colour_params(ppath, metadata);
        result["colour_pipeline"] = colour_p;
        result["colour_pipeline"]["path"] = path;

        // Extract show/seq/shot for external metadata
        auto show = extract_show_from_path(ppath);
        auto [seq, shot] = extract_seq_shot_from_path(ppath);

        if (!show.empty()) {
            result["metadata"]["external"]["RodeoFX"]["show"] = show;
        }
        if (!seq.empty()) {
            result["metadata"]["external"]["RodeoFX"]["seq"] = seq;
        }
        if (!shot.empty()) {
            result["metadata"]["external"]["RodeoFX"]["shot"] = shot;
        }

        spdlog::debug(
            "RodeoMediaHook::modify_metadata colour_pipeline={}",
            result["colour_pipeline"].dump());

        return result;
    }

  private:
    module::BooleanAttribute *auto_trim_slate_;
    module::StringAttribute *rez_ocio_config_root_;
    module::StringAttribute *default_show_;

    // File extension sets for media type detection
    static inline const std::set<std::string> movie_ext_{".mov", ".mp4", ".mxf", ".qt"};
    static inline const std::set<std::string> still_ext_{
        ".tiff", ".tif", ".jpeg", ".jpg", ".png"};

    /**
     * Check if a path should have its slate frame trimmed.
     *
     * @param path POSIX path to the media file
     * @return true if slate should be trimmed (MOV in /movhistory/ or /.published/)
     */
    bool should_trim_slate(const std::string &path) {
        // Only MOV-type containers
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (movie_ext_.find(ext) == movie_ext_.end())
            return false;

        // Only if path contains /movhistory/ or /.published/
        if (path.find("/movhistory/") != std::string::npos)
            return true;
        if (path.find("/.published/") != std::string::npos)
            return true;

        return false;
    }

    /**
     * Extract show code from RodeoFX paths.
     *
     * Patterns:
     *   /rdo/shows/{show}/...
     *   /shows/{show}/...
     *
     * @param path POSIX path
     * @return Show code (lowercase) or empty string
     */
    std::string extract_show_from_path(const std::string &path) {
        static const std::regex show_regex(R"(/(?:rdo/)?shows/([^/]+)/)");

        std::smatch match;
        if (std::regex_search(path, match, show_regex)) {
            std::string show = match[1].str();
            std::transform(show.begin(), show.end(), show.begin(), ::tolower);
            return show;
        }
        return "";
    }

    /**
     * Extract SEQ and SHOT from RodeoFX paths.
     *
     * Supported patterns:
     *   1. /shows/SHOW/SEQ/SHOT/ where SHOT is 4 digits
     *   2. /shows/SHOW/SEQ/SEQ_SHOT/ (full format like jtz/jtz_5010)
     *   3. /.published/SEQ/SEQ_SHOT/
     *   4. Filename pattern: SEQ_SHOT_*.mov
     *   5. 6-digit SEQ in path: /505113/505113_0010/
     *
     * @param path POSIX path
     * @return Pair of (seq, shot) strings, empty if not found
     */
    std::pair<std::string, std::string> extract_seq_shot_from_path(const std::string &path) {
        if (path.empty())
            return {"", ""};

        std::smatch match;

        // Pattern 1: /shows/SHOW/SEQ/SHOT/ where SEQ is alphanumeric and SHOT is 4 digits
        // Returns full seq_shot format (e.g., "205042_0130")
        static const std::regex pattern1(R"(/shows/[^/]+/([a-zA-Z0-9]+)/(\d{4})/)");
        if (std::regex_search(path, match, pattern1)) {
            std::string seq = match[1].str();
            std::string shot_num = match[2].str();
            return {seq, seq + "_" + shot_num};
        }

        // Pattern 2: /shows/SHOW/SEQ/SEQ_SHOT/ (full format)
        static const std::regex pattern2(R"(/shows/[^/]+/([a-zA-Z0-9]+)/([a-zA-Z0-9]+_\d+)/)");
        if (std::regex_search(path, match, pattern2)) {
            return {match[1].str(), match[2].str()};
        }

        // Pattern 3: /.published/SEQ/SEQ_SHOT/
        static const std::regex pattern3(R"(/\.published/([a-zA-Z0-9]+)/([a-zA-Z0-9]+_\d+)/)");
        if (std::regex_search(path, match, pattern3)) {
            return {match[1].str(), match[2].str()};
        }

        // Pattern 4: Filename pattern SEQ_SHOT_*.mov
        // Returns full seq_shot format (e.g., "205042_0130")
        std::string basename = fs::path(path).filename().string();
        static const std::regex pattern4(R"(^([a-zA-Z0-9]+)_(\d{4})[._])");
        if (std::regex_search(basename, match, pattern4)) {
            std::string seq = match[1].str();
            std::string shot_num = match[2].str();
            return {seq, seq + "_" + shot_num};
        }

        // Pattern 5: 6-digit SEQ in path /SEQ/SEQ_SHOT/
        static const std::regex pattern5(R"(/([a-zA-Z0-9]{6})/([a-zA-Z0-9]+_\d{4})/)");
        if (std::regex_search(path, match, pattern5)) {
            return {match[1].str(), match[2].str()};
        }

        return {"", ""};
    }

    /**
     * Check if media is "baked" (movie or still image that needs raw passthrough).
     *
     * @param path Path to the media file
     * @return true if file is a movie or still image
     */
    bool is_baked_media(const std::string &path) {
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (movie_ext_.find(ext) != movie_ext_.end())
            return true;
        if (still_ext_.find(ext) != still_ext_.end())
            return true;

        return false;
    }

    /**
     * Check if path is a lineup EXR that needs the non-white-balance view.
     *
     * @param path Path to check
     * @return true if path contains ".lineup." and is an EXR
     */
    bool is_lineup_exr(const std::string &path) {
        std::string lower_path = path;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

        if (!ends_with(lower_path, ".exr"))
            return false;

        return lower_path.find(".lineup.") != std::string::npos;
    }

    /**
     * Check if path is an asset (vs a shot).
     *
     * @param path Path to check
     * @return true if path contains "/assets/"
     */
    bool is_asset(const std::string &path) {
        std::string lower_path = path;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
        return lower_path.find("/assets/") != std::string::npos;
    }

    /**
     * Get the view override based on media type.
     * Only used for baked media (MOV/stills) that need raw passthrough.
     * EXR views are handled via automatic_view instead.
     *
     * @param path Path to media file
     * @return Pair of (view_name, needs_raw_input) - view_name empty if no override needed
     */
    std::pair<std::string, bool> get_override_view_for_path(const std::string &path) {
        if (is_baked_media(path)) {
            return {"raw", true};
        }

        return {"", false};
    }

    /**
     * Find the OCIO config path for a show.
     *
     * @param show Show code
     * @return Path to OCIO config file or empty string if not found
     */
    std::string find_ocio_config(const std::string &show) {
        if (show.empty())
            return "";

        std::string show_lower = show;
        std::transform(show_lower.begin(), show_lower.end(), show_lower.begin(), ::tolower);

        // RodeoFX OCIO config path pattern
        std::string config_path = fmt::format(
            "/rdo/shows/{}/_project_ref/_imaging/_ocio2_xstudio/{}_config.ocio",
            show_lower,
            show_lower);

        if (fs::exists(config_path)) {
            return config_path;
        }

        return "";
    }

    /**
     * Build the colour pipeline parameters JSON.
     *
     * This sets up:
     *   - ocio_context with SEQ, SHOT, REZ_RDO_OCIO_CONFIG_ROOT, RDO_CURRENT_SHOW
     *   - ocio_config path
     *   - override_view for MOVs/stills ("raw") or lineup EXRs
     *   - input_colorspace for MOVs/stills ("Utility - Raw")
     *
     * @param path POSIX path to media
     * @param metadata Existing metadata
     * @return JsonStore with colour_pipeline parameters
     */
    utility::JsonStore
    colour_params(const std::string &path, const utility::JsonStore &metadata) {
        utility::JsonStore r(R"({})"_json);

        auto context = R"({})"_json;

        // Extract SEQ/SHOT from path
        auto [seq, shot] = extract_seq_shot_from_path(path);

        // Fallback to environment variables if path extraction failed
        if (seq.empty()) {
            const char *env_seq = std::getenv("SEQ");
            if (env_seq && *env_seq) {
                seq = env_seq;
            }
        }
        if (shot.empty()) {
            const char *env_shot = std::getenv("SHOT");
            if (env_shot && *env_shot) {
                shot = env_shot;
            }
        }

        // Extract show from path
        std::string show = extract_show_from_path(path);

        // Fallback to environment variable
        if (show.empty()) {
            const char *env_show = std::getenv("RDO_CURRENT_SHOW");
            if (env_show && *env_show) {
                show = env_show;
            }
        }

        // Fallback to default show preference
        if (show.empty()) {
            show = default_show_->value();
        }

        // Get REZ OCIO config root from preference or environment
        std::string rez_root = rez_ocio_config_root_->value();
        const char *env_rez_root = std::getenv("REZ_RDO_OCIO_CONFIG_ROOT");
        if (env_rez_root && *env_rez_root) {
            rez_root = env_rez_root;
        }

        // Build OCIO context
        if (!seq.empty()) {
            context["SEQ"] = seq;
        }
        if (!shot.empty()) {
            context["SHOT"] = shot;
        }
        if (!rez_root.empty()) {
            context["REZ_RDO_OCIO_CONFIG_ROOT"] = rez_root;
        }
        if (!show.empty()) {
            context["RDO_CURRENT_SHOW"] = show;
        }

        // Debug: log the OCIO context being applied
        bool is_lineup = is_lineup_exr(path);
        spdlog::debug(
            "RodeoMediaHook::colour_params OCIO context: path={} show={} seq={} shot={} "
            "is_lineup={} (lineup files use rdo-nwb to bypass shot white balance)",
            path,
            show.empty() ? "(none)" : show,
            seq.empty() ? "(none)" : seq,
            shot.empty() ? "(none)" : shot,
            is_lineup);

        // Only set OCIO config if we have a show
        if (!show.empty()) {
            r["ocio_context"] = context;

            std::string ocio_config = find_ocio_config(show);
            if (!ocio_config.empty()) {
                r["ocio_config"] = ocio_config;
            }

            // Set working space to scene_linear (standard for EXR workflows)
            r["working_space"] = "scene_linear";

            // Use OCIO file_rules to determine input colorspace for ALL media types
            // The config's file_rules handle: Stills, Movies, Lineup, Assets, Default
            // For raw/data colorspaces, also set raw view for full passthrough
            std::string input_cs = "(none)";
            std::string auto_view = "(none)";
            try {
                auto config = OCIO::Config::CreateFromFile(ocio_config.c_str());
                const char *cs = config->getColorSpaceFromFilepath(path.c_str());
                if (cs && *cs) {
                    r["input_colorspace"] = cs;
                    input_cs = cs;
                    spdlog::debug(
                        "RodeoMediaHook: OCIO file_rules matched '{}' for path: {}",
                        cs,
                        path);

                    // Check if colorspace is raw/data - if so, use raw view for passthrough
                    // This ensures baked content (MOVs, stills) displays without transforms
                    auto colorspace = config->getColorSpace(cs);
                    if (colorspace) {
                        const char *encoding = colorspace->getEncoding();
                        std::string csName(cs);
                        // Check for data encoding or raw in name
                        bool is_raw = (encoding && std::string(encoding) == "data") ||
                                      (csName.find("Raw") != std::string::npos) ||
                                      (csName.find("raw") != std::string::npos);
                        if (is_raw) {
                            // Look for raw view in the config
                            const char *defaultDisplay = config->getDefaultDisplay();
                            if (defaultDisplay) {
                                int numViews = config->getNumViews(defaultDisplay);
                                for (int i = 0; i < numViews; ++i) {
                                    const char *viewName = config->getView(defaultDisplay, i);
                                    if (viewName && std::string(viewName) == "raw") {
                                        r["automatic_view"] = "raw";
                                        auto_view = "raw";
                                        spdlog::debug(
                                            "RodeoMediaHook: Using raw view for passthrough");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception &e) {
                spdlog::warn(
                    "RodeoMediaHook: Failed to get colorspace from OCIO file_rules: {}",
                    e.what());
            }
            spdlog::debug(
                "RodeoMediaHook::colour_params path={} show={} seq={} shot={} "
                "ocio_config={} input_colorspace={} automatic_view={}",
                path,
                show,
                seq,
                shot,
                ocio_config.empty() ? "(none)" : ocio_config,
                input_cs,
                auto_view);
        } else {
            // No show context - use raw passthrough
            r["ocio_config"]   = "__raw__";
            r["working_space"] = "raw";
            spdlog::debug(
                "RodeoMediaHook::colour_params path={} - no show found, using raw passthrough",
                path);
        }

        return r;
    }
};

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<MediaHookPlugin<MediaHookActor<RodeoMediaHook>>>(
                Uuid("a7b8c9d0-e1f2-4a5b-8c7d-9e0f1a2b3c4d"),
                "RodeoFX",
                "xStudio",  // Use "xStudio" as author to auto-enable plugin
                "RodeoFX Media Hook",
                semver::version("1.0.0"))}));
}
}
