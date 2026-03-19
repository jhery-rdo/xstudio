// SPDX-License-Identifier: Apache-2.0
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <set>
#include <unordered_map>
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


#ifdef __APPLE__
        // Detect macOS mount type for path remapping.
        // OCIO configs reference /shows/... search paths (Linux convention).
        // On macOS: NFS mounts at /rdo/shows, Samba mounts at /Volumes/shows.
        if (fs::is_directory("/Volumes/shows")) {
            shows_mount_prefix_ = "/Volumes";
        } else if (fs::is_directory("/rdo/shows")) {
            shows_mount_prefix_ = "/rdo";
        }
        spdlog::info("RodeoMediaHook: macOS mount prefix: {}", shows_mount_prefix_);
#endif
    }

    ~RodeoMediaHook() override = default;

    /**
     * Modify media reference to trim slate frames from published MOVs.
     */
    std::optional<utility::MediaReference> modify_media_reference(
        const utility::MediaReference &mr, const utility::JsonStore &jsn) override {

        utility::MediaReference result = mr;
        bool changed                   = false;

        // Pre-warm the OCIO config for this show so modify_metadata is fast.
        // modify_media_reference runs before the colour pipeline evaluates;
        // without this, the first QT load shows the wrong look while the
        // OCIO config is being created over Samba.
        {
            auto path = normalize_path(uri_to_posix_path(mr.uri()));
            auto show = extract_show_from_path(path);
            if (!show.empty()) {
                find_ocio_config(show);
            }
        }

        if (auto_trim_slate_->value() && mr.container()) {
            auto path = normalize_path(uri_to_posix_path(mr.uri()));

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
        auto ppath             = normalize_path(uri_to_posix_path(uri));

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

    // Cache OCIO configs per config file path (avoids re-parsing on every media load)
    std::unordered_map<std::string, OCIO::ConstConfigRcPtr> ocio_config_cache_;

    // Cache resolved OCIO config paths per show (avoids NFS stat on every media load)
    std::unordered_map<std::string, std::string> ocio_config_path_cache_;

    // macOS mount prefix: "/rdo" for NFS, "/Volumes" for Samba
    std::string shows_mount_prefix_{"/rdo"};

    // Cache for macOS-fixed OCIO config paths (original -> fixed temp path)
    std::unordered_map<std::string, std::string> fixed_config_cache_;

    // File extension sets for media type detection
    static inline const std::set<std::string> movie_ext_{".mov", ".mp4", ".mxf", ".qt"};

    /**
     * Normalize media paths to use the current macOS mount prefix.
     * Media paths from OTIO/database use /rdo/shows/ (Linux/NFS convention).
     * On macOS with Samba, the actual mount is at /Volumes/shows/.
     */
    std::string normalize_path(const std::string &path) const {
        if (shows_mount_prefix_ == "/rdo")
            return path;

        // Remap /rdo/shows/... to {mount_prefix}/shows/...
        if (path.size() >= 11 && path.substr(0, 11) == "/rdo/shows/") {
            return shows_mount_prefix_ + path.substr(4);
        }
        return path;
    }
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
     * Get an OCIO config, returning a cached instance if available.
     * Avoids re-parsing the same config file on every media load.
     *
     * @param config_path Path to the OCIO config file
     * @return Cached or newly parsed OCIO config
     */
    OCIO::ConstConfigRcPtr get_ocio_config_cached(const std::string &config_path) {
        auto it = ocio_config_cache_.find(config_path);
        if (it != ocio_config_cache_.end()) {
            return it->second;
        }
        auto config = OCIO::Config::CreateFromFile(config_path.c_str());
        ocio_config_cache_[config_path] = config;
        return config;
    }

    /**
     * Find the OCIO config path for a show.
     * Results are cached per show to avoid repeated NFS stat calls.
     * On macOS, returns a fixed config with corrected search paths.
     *
     * @param show Show code
     * @return Path to OCIO config file or empty string if not found
     */
    std::string find_ocio_config(const std::string &show) {
        if (show.empty())
            return "";

        auto it = ocio_config_path_cache_.find(show);
        if (it != ocio_config_path_cache_.end()) {
            return it->second;
        }

        std::string show_lower = show;
        std::transform(show_lower.begin(), show_lower.end(), show_lower.begin(), ::tolower);

        // RodeoFX OCIO config path pattern
        std::string config_path = fmt::format(
            "{}/shows/{}/_project_ref/_imaging/_ocio2_xstudio/{}_config.ocio",
            shows_mount_prefix_,
            show_lower,
            show_lower);

        std::string result;
        if (fs::exists(config_path)) {
#ifdef __APPLE__
            result = fix_ocio_config_for_macos(config_path, show_lower);
#else
            result = config_path;
#endif
        }

        ocio_config_path_cache_[show] = result;
        return result;
    }

#ifdef __APPLE__
    /**
     * Fix OCIO config search paths for macOS.
     *
     * OCIO configs use /shows/... search paths (Linux convention).
     * On macOS, /shows/ doesn't exist - the mount is at /rdo/shows/ (NFS)
     * or /Volumes/shows/ (Samba). This creates a modified config with
     * corrected search paths and writes it to /tmp/.
     *
     * @param original_path Path to the original OCIO config
     * @param show Show code (used for temp file naming)
     * @return Path to the fixed config (or original if fix fails)
     */
    std::string fix_ocio_config_for_macos(
        const std::string &original_path, const std::string &show) {

        std::string temp_path = "/tmp/xstudio_ocio_" + show + "_config.ocio";

        auto it = fixed_config_cache_.find(original_path);
        if (it != fixed_config_cache_.end()) {
            return it->second;
        }

        // Fast path: config pre-created by Python startup (avoids slow Samba I/O)
        if (fs::exists(temp_path)) {
            spdlog::info(
                "RodeoMediaHook: Using pre-warmed OCIO config for {}: {}", show, temp_path);
            fixed_config_cache_[original_path] = temp_path;
            return temp_path;
        }

        try {
            auto config  = OCIO::Config::CreateFromFile(original_path.c_str());
            auto econfig = config->createEditableCopy();

            // Temp directory for fixed symlinks
            std::string temp_dir = "/tmp/xstudio_ocio_" + show;
            fs::create_directories(temp_dir);

            // Collect remapped search paths and fix broken symlinks
            std::vector<std::string> fixed_paths;
            for (int i = 0; i < config->getNumSearchPaths(); ++i) {
                std::string sp = config->getSearchPath(i);
                if (sp.size() >= 7 && sp.substr(0, 7) == "/shows/") {
                    sp = shows_mount_prefix_ + sp;
                }
                fixed_paths.push_back(sp);

                // Resolve known variables to check for broken symlinks.
                // SEQ/SHOT vary per media item so skip those paths.
                std::string resolved_sp = sp;
                auto pos = resolved_sp.find("${RDO_CURRENT_SHOW}");
                if (pos != std::string::npos) {
                    resolved_sp.replace(pos, 19, show);
                }
                if (resolved_sp.find("${") == std::string::npos &&
                    fs::is_directory(resolved_sp)) {
                    fix_broken_symlinks(resolved_sp, temp_dir);
                }
            }

            // Add temp dir first so fixed symlinks take priority
            econfig->clearSearchPaths();
            econfig->addSearchPath(temp_dir.c_str());
            for (const auto &sp : fixed_paths) {
                econfig->addSearchPath(sp.c_str());
            }

            // Write fixed config to temp file
            std::string temp_path =
                "/tmp/xstudio_ocio_" + show + "_config.ocio";
            std::ofstream out(temp_path);
            if (!out.is_open()) {
                spdlog::warn(
                    "RodeoMediaHook: Cannot write fixed OCIO config to {}", temp_path);
                fixed_config_cache_[original_path] = original_path;
                return original_path;
            }
            econfig->serialize(out);
            out.close();

            spdlog::info(
                "RodeoMediaHook: Fixed OCIO config for {}: /shows/ -> {}/shows/ ({})",
                show, shows_mount_prefix_, temp_path);

            fixed_config_cache_[original_path] = temp_path;
            return temp_path;

        } catch (const std::exception &e) {
            spdlog::warn(
                "RodeoMediaHook: Failed to fix OCIO config {}: {}", original_path, e.what());
            fixed_config_cache_[original_path] = original_path;
            return original_path;
        }
    }

    /**
     * Scan a directory for broken symlinks and create fixed copies in temp_dir.
     * Remaps symlink targets from /shows/... to <mount_prefix>/shows/...
     */
    void fix_broken_symlinks(const std::string &dir_path, const std::string &temp_dir) {
        try {
            for (const auto &entry : fs::directory_iterator(dir_path)) {
                if (!fs::is_symlink(entry.path())) {
                    continue;
                }

                auto target = fs::read_symlink(entry.path());
                std::string target_str = target.string();

                // Check if symlink target starts with /shows/ (Linux path)
                if (target_str.size() >= 7 && target_str.substr(0, 7) == "/shows/") {
                    std::string fixed_target = shows_mount_prefix_ + target_str;

                    if (fs::exists(fixed_target)) {
                        std::string link_name = entry.path().filename().string();
                        fs::path fixed_link = fs::path(temp_dir) / link_name;

                        // Remove existing link if present
                        fs::remove(fixed_link);
                        fs::create_symlink(fixed_target, fixed_link);

                        spdlog::info(
                            "RodeoMediaHook: Fixed broken symlink: {} -> {}",
                            link_name, fixed_target);
                    }
                }
            }
        } catch (const std::exception &e) {
            spdlog::warn(
                "RodeoMediaHook: Error scanning for broken symlinks in {}: {}",
                dir_path, e.what());
        }
    }
#endif

    /**
     * Build the colour pipeline parameters JSON.
     *
     * This sets up:
     *   - ocio_context with SEQ, SHOT, REZ_RDO_OCIO_CONFIG_ROOT, RDO_CURRENT_SHOW
     *   - ocio_config path
     *   - input_colorspace via OCIO file_rules
     *
     * All view/look decisions are driven by the OCIO config.
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

        spdlog::debug(
            "RodeoMediaHook::colour_params OCIO context: path={} show={} seq={} shot={}",
            path,
            show.empty() ? "(none)" : show,
            seq.empty() ? "(none)" : seq,
            shot.empty() ? "(none)" : shot);

        // Set OCIO config and let the config drive all colour decisions
        if (!show.empty()) {
            r["ocio_context"] = context;

            std::string ocio_config = find_ocio_config(show);
            if (!ocio_config.empty()) {
                r["ocio_config"] = ocio_config;
            }

            // Use OCIO file_rules to determine input colorspace
            std::string input_cs = "(none)";
            try {
                auto config = get_ocio_config_cached(ocio_config);
                const char *cs = config->getColorSpaceFromFilepath(path.c_str());
                if (cs && *cs) {
                    r["input_colorspace"] = cs;
                    input_cs = cs;
                    spdlog::debug(
                        "RodeoMediaHook: OCIO file_rules matched '{}' for path: {}",
                        cs,
                        path);

                    // Raw colorspaces need raw view passthrough - otherwise
                    // the default view (client look) gets applied incorrectly
                    std::string csName(cs);
                    if (csName.find("Raw") != std::string::npos ||
                        csName.find("raw") != std::string::npos) {
                        r["automatic_view"] = "raw";
                    }
                    // Asset EXRs: find a neutral view from the config
                    else if (path.find("/_asset/") != std::string::npos ||
                             path.find("/.published/assets/") != std::string::npos) {
                        auto display = config->getDefaultDisplay();
                        int nv = config->getNumViews(display, cs);
                        for (int vi = 0; vi < nv; ++vi) {
                            std::string view = config->getView(display, cs, vi);
                            std::string vl = view;
                            std::transform(vl.begin(), vl.end(), vl.begin(), ::tolower);
                            if (vl.find("neutral") != std::string::npos) {
                                r["automatic_view"] = view;
                                break;
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
                "ocio_config={} input_colorspace={}",
                path,
                show,
                seq,
                shot,
                ocio_config.empty() ? "(none)" : ocio_config,
                input_cs);
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
