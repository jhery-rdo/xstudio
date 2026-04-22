// SPDX-License-Identifier: Apache-2.0
//
// RDO Media Loader — C++ fast-path for adding media from the Python RDO Browser.
//
// Instead of multiple synchronous Python→C++ round-trips, the Python side sends
// a single fire-and-forget message with a JSON payload. This plugin spawns all
// actors directly in C++, eliminating actor queue contention.
//
// Usage from Python:
//   loader = connection.get_actor_from_registry("RDO_MEDIA_LOADER")
//   connection.send(loader.remote, playlist::add_media_atom(),
//       JsonStore(payload), playlist_actor, subset_actor, FrameRate(24.0))

// cpp-httplib feature flags must be set before httplib.h is included (directly
// or transitively by any other header) so the SSL-capable Client constructor
// is compiled in.  Without this, https:// URLs raise
// "'https' scheme is not supported" at runtime.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#ifndef CPPHTTPLIB_ZLIB_SUPPORT
#define CPPHTTPLIB_ZLIB_SUPPORT
#endif

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

#include "xstudio/atoms.hpp"
#include "xstudio/http_client/http_client.hpp"
#include "xstudio/media/media_actor.hpp"
#include "xstudio/plugin_manager/plugin_base.hpp"
#include "xstudio/plugin_manager/plugin_manager.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/utility/uuid.hpp"

using namespace xstudio;
using namespace xstudio::utility;

namespace {

const auto RdoMediaLoaderRegistry = std::string("RDO_MEDIA_LOADER");

// Source identifiers (user-visible names shown on Media sources in xStudio).
const auto SourceMov    = std::string("MOV");
const auto SourceFrames = std::string("Frames");
const auto SourceWeb    = std::string("Web");

// ---------------------------------------------------------------------------
// Web (http/https) download support
// ---------------------------------------------------------------------------
// xStudio's media reader only speaks filesystem paths.  The ShotGrid
// sg_uploaded_movie_mp4 field returns a short-lived S3 signed URL; before we
// can hand it to MediaSourceActor we need to materialise it on disk.  The
// download is a straight HTTP(S) GET via cpp-httplib (already pulled in as a
// dep of xstudio::http_client).  Files land in a per-session cache dir that
// is wiped at plugin startup.

std::string web_cache_dir() {
    return (std::filesystem::temp_directory_path() / "xstudio_rdo_media_loader").string();
}

bool is_http_url(const std::string &path) {
    return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
}

void cleanup_web_cache() {
    try {
        auto dir = web_cache_dir();
        if (std::filesystem::exists(dir)) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            if (ec) {
                spdlog::warn(
                    "RdoMediaLoader: web cache cleanup failed ({}): {}",
                    dir, ec.message());
            } else {
                spdlog::info("RdoMediaLoader: cleared web cache at {}", dir);
            }
        }
    } catch (const std::exception &e) {
        spdlog::warn("RdoMediaLoader: cleanup exception: {}", e.what());
    }
}

// Download a web URL to the cache dir; returns the local path on success, an
// empty string on failure.  The target filename is derived from the last path
// component of the URL (ShotGrid prefixes uploaded mp4s with a content hash,
// so same-content reuse is safe across signed-URL rotations within a session).
std::string download_web_url(const std::string &url) {
    try {
        auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) return std::string();
        auto host_start = scheme_end + 3;
        auto path_start = url.find('/', host_start);
        if (path_start == std::string::npos) return std::string();

        std::string scheme_host     = url.substr(0, path_start);
        std::string path_with_query = url.substr(path_start);

        // Derive filename from the path part only (strip query string).
        auto q = path_with_query.find('?');
        std::string path_no_query =
            (q != std::string::npos) ? path_with_query.substr(0, q) : path_with_query;
        auto last_slash = path_no_query.rfind('/');
        std::string filename =
            (last_slash != std::string::npos && last_slash + 1 < path_no_query.size())
                ? path_no_query.substr(last_slash + 1)
                : std::string("web.mp4");
        if (filename.empty()) filename = "web.mp4";

        auto cache_dir = web_cache_dir();
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        if (ec) {
            spdlog::warn(
                "RdoMediaLoader: cannot create cache dir {}: {}",
                cache_dir, ec.message());
            return std::string();
        }

        auto local_path = (std::filesystem::path(cache_dir) / filename).string();

        // Reuse an already-downloaded file when the filename matches.  ShotGrid
        // uploaded mp4s are named by a content hash so the same filename implies
        // the same bytes; the signed URL query string rotates but we key purely
        // off the path portion.
        if (std::filesystem::exists(local_path) &&
            std::filesystem::file_size(local_path) > 0) {
            spdlog::info(
                "RdoMediaLoader: reusing cached web file {}", local_path);
            return local_path;
        }

        spdlog::info(
            "RdoMediaLoader: downloading web file -> {}", local_path);

        httplib::Client cli(scheme_host);
        cli.set_follow_location(true);
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(60, 0);

        std::ofstream out(local_path, std::ios::binary);
        if (!out) {
            spdlog::warn(
                "RdoMediaLoader: cannot open {} for write", local_path);
            return std::string();
        }

        // Stream directly to disk so we don't buffer a multi-MB body in memory.
        auto res = cli.Get(
            path_with_query.c_str(),
            [&out](const char *data, size_t len) {
                out.write(data, static_cast<std::streamsize>(len));
                return static_cast<bool>(out);
            });
        out.close();

        if (!res) {
            spdlog::warn(
                "RdoMediaLoader: download error: {}",
                httplib::to_string(res.error()));
            std::filesystem::remove(local_path, ec);
            return std::string();
        }
        if (res->status != 200) {
            spdlog::warn(
                "RdoMediaLoader: download http status={} for {}",
                res->status, filename);
            std::filesystem::remove(local_path, ec);
            return std::string();
        }

        auto sz = std::filesystem::file_size(local_path, ec);
        spdlog::info(
            "RdoMediaLoader: downloaded {} ({} bytes)",
            filename, ec ? 0 : static_cast<long long>(sz));
        return local_path;

    } catch (const std::exception &e) {
        spdlog::warn("RdoMediaLoader: download exception: {}", e.what());
        return std::string();
    }
}

// Small holder for a resolved representation during loading.
struct Representation {
    std::string kind;          // SourceMov / SourceFrames / SourceWeb
    std::string path;
    std::string frame_range;   // only meaningful for Frames
};

// Worker actor — follows DNEG's promise-chaining pattern.
// Step 1: create primary source → Step 2: assemble media → Step 3: add secondaries.
class MediaLoaderWorker : public caf::event_based_actor {
  public:
    MediaLoaderWorker(caf::actor_config &cfg)
        : caf::event_based_actor(cfg) {}

    const char *name() const override { return "RdoMediaLoaderWorker"; }

    caf::behavior make_behavior() override {
        return {
            [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {},

            // Main entry: create media and add to playlist.
            [=](playlist::add_media_atom,
                const JsonStore &payload,
                caf::actor playlist,
                caf::actor subset,
                const FrameRate &rate) {
                try {
                    do_load(payload, playlist, subset, rate);
                } catch (const std::exception &e) {
                    spdlog::warn("RdoMediaLoader: {}", e.what());
                }
            },

            // Movie source creation handler (used by MOV and Web).
            [=](media::add_media_source_atom,
                const std::string &source_name,
                const std::string &path,
                const FrameRate &rate,
                bool /*is_movie*/) -> caf::result<UuidActor> {
                auto rp = make_response_promise<UuidActor>();
                if (path.empty()) {
                    rp.deliver(UuidActor());
                    return rp;
                }
                try {
                    auto uri         = posix_path_to_uri(path);
                    auto source_uuid = Uuid::generate();
                    auto source =
                        spawn<media::MediaSourceActor>(source_name, uri, rate, source_uuid);
                    mail(media::acquire_media_detail_atom_v, rate)
                        .request(source, std::chrono::seconds(30))
                        .then(
                            [=](bool) mutable {
                                rp.deliver(UuidActor(source_uuid, source));
                            },
                            [=](caf::error &err) mutable {
                                spdlog::warn(
                                    "RdoMediaLoader: {} detail: {}",
                                    source_name, to_string(err));
                                rp.deliver(UuidActor(source_uuid, source));
                            });
                } catch (const std::exception &e) {
                    spdlog::warn("RdoMediaLoader: {}: {}", source_name, e.what());
                    rp.deliver(UuidActor());
                }
                return rp;
            },

            // Frames (image sequence) source creation handler.
            [=](media::add_media_source_atom,
                const std::string &source_name,
                const std::string &path,
                const std::string &frame_range,
                const FrameRate &rate) -> caf::result<UuidActor> {
                auto rp = make_response_promise<UuidActor>();
                if (path.empty()) {
                    rp.deliver(UuidActor());
                    return rp;
                }
                try {
                    FrameList frame_list;
                    caf::uri uri;

                    // Convert Python {:04d} format to #### for xStudio parser.
                    auto native_path = path;
                    static const std::regex pyformat(R"(\{:\d*d\})");
                    native_path = std::regex_replace(native_path, pyformat, "####");

                    if (!frame_range.empty()) {
                        frame_list = FrameList(frame_range);
                        uri = parse_cli_posix_path(native_path, frame_list, false);
                    } else {
                        uri = parse_cli_posix_path(native_path, frame_list, true);
                    }

                    auto source_uuid = Uuid::generate();
                    auto source =
                        frame_list.empty()
                            ? spawn<media::MediaSourceActor>(
                                  source_name, uri, rate, source_uuid)
                            : spawn<media::MediaSourceActor>(
                                  source_name, uri, frame_list, rate, source_uuid);

                    mail(media::acquire_media_detail_atom_v, rate)
                        .request(source, std::chrono::seconds(30))
                        .then(
                            [=](bool) mutable {
                                rp.deliver(UuidActor(source_uuid, source));
                            },
                            [=](caf::error &err) mutable {
                                spdlog::warn(
                                    "RdoMediaLoader: {} detail: {}",
                                    source_name, to_string(err));
                                rp.deliver(UuidActor(source_uuid, source));
                            });
                } catch (const std::exception &e) {
                    spdlog::warn("RdoMediaLoader: {}: {}", source_name, e.what());
                    rp.deliver(UuidActor());
                }
                return rp;
            },
        };
    }

  private:
    void do_load(
        const JsonStore &payload,
        caf::actor playlist,
        caf::actor subset,
        const FrameRate &rate) {

        auto movie_path  = payload.value("movie_path", std::string());
        auto frames_path = payload.value("frames_path", std::string());
        auto web_path    = payload.value("web_path", std::string());
        auto frame_range = payload.value("frame_range", std::string());
        auto media_name  = payload.value("version_name", std::string("unknown"));
        auto set_viewer  = payload.value("set_viewer", false);
        auto preference  = payload.value("media_preference", std::string(SourceMov));

        // Web is only loaded when it is the explicit preference — otherwise we
        // drop it to avoid the HTTP fetch cost on every media item. xStudio's
        // media reader cannot consume http(s) URIs directly, so a remote Web
        // URL is downloaded to the session-local cache here. Failure returns
        // an empty path, which causes build_load_order() to drop Web and fall
        // through to MOV/Frames.
        if (preference == SourceWeb) {
            if (!web_path.empty() && is_http_url(web_path)) {
                web_path = download_web_url(web_path);
            }
        } else {
            web_path.clear();
        }

        // Build the ordered list of available representations:
        // the first entry becomes the primary, the rest are added as secondaries
        // (in order) once the primary has been attached to the media actor.
        std::vector<Representation> order = build_load_order(
            preference, movie_path, frames_path, web_path, frame_range);

        if (order.empty()) {
            spdlog::warn("RdoMediaLoader: no sources for {}", media_name);
            return;
        }

        const auto primary = order.front();
        std::vector<Representation> secondaries(order.begin() + 1, order.end());

        spdlog::info(
            "RdoMediaLoader: loading {} as primary for {} (preference={})",
            primary.kind, media_name, preference);

        request_source(
            primary, rate,
            [=, this](const UuidActor &primary_ua) mutable {
                if (primary_ua.uuid().is_null()) {
                    // Primary creation failed — try the next representation
                    // as a fallback primary (e.g. MOV fails → Frames).
                    if (secondaries.empty()) {
                        spdlog::warn(
                            "RdoMediaLoader: primary ({}) failed for {}, no fallback",
                            primary.kind, media_name);
                        return;
                    }
                    spdlog::info(
                        "RdoMediaLoader: {} failed, falling back to {} for {}",
                        primary.kind, secondaries.front().kind, media_name);
                    auto new_primary = secondaries.front();
                    std::vector<Representation> new_secondaries(
                        secondaries.begin() + 1, secondaries.end());
                    request_source(
                        new_primary, rate,
                        [=, this](const UuidActor &fallback_ua) mutable {
                            if (fallback_ua.uuid().is_null()) {
                                spdlog::warn(
                                    "RdoMediaLoader: all sources failed for {}",
                                    media_name);
                                return;
                            }
                            add_to_playlist(
                                fallback_ua, media_name, set_viewer, playlist,
                                subset, payload, new_secondaries, rate);
                        },
                        [=](caf::error &err) {
                            spdlog::warn(
                                "RdoMediaLoader: fallback primary: {}",
                                to_string(err));
                        });
                    return;
                }
                add_to_playlist(
                    primary_ua, media_name, set_viewer, playlist, subset,
                    payload, secondaries, rate);
            },
            [=](caf::error &err) {
                spdlog::warn(
                    "RdoMediaLoader: primary step failed: {}", to_string(err));
            });
    }

    // Build an ordered list of available representations, with the user-preferred
    // one first. Missing paths are skipped.
    std::vector<Representation> build_load_order(
        const std::string &preference,
        const std::string &movie_path,
        const std::string &frames_path,
        const std::string &web_path,
        const std::string &frame_range) const {

        // Canonical order when the preferred representation is missing.
        // Web is intentionally excluded from the fallback chain: it is only
        // loaded when the user explicitly prefers it, so we don't pay the
        // HTTP download cost on every media item.
        const std::vector<std::string> fallback = {SourceMov, SourceFrames};

        // Start with the preferred representation, then append fallbacks that
        // aren't the preference.
        std::vector<std::string> kind_order;
        kind_order.reserve(3);
        kind_order.push_back(preference);
        for (const auto &k : fallback) {
            if (k != preference) kind_order.push_back(k);
        }

        std::vector<Representation> out;
        out.reserve(3);
        for (const auto &kind : kind_order) {
            if (kind == SourceMov && !movie_path.empty()) {
                out.push_back({SourceMov, movie_path, std::string()});
            } else if (kind == SourceFrames && !frames_path.empty()) {
                out.push_back({SourceFrames, frames_path, frame_range});
            } else if (kind == SourceWeb && !web_path.empty()) {
                out.push_back({SourceWeb, web_path, std::string()});
            }
        }
        return out;
    }

    // Request creation of a media source for the given representation, dispatching
    // to the movie handler (MOV/Web) or the sequence handler (Frames).
    template <typename OnSuccess, typename OnError>
    void request_source(
        const Representation &rep,
        const FrameRate &rate,
        OnSuccess &&on_success,
        OnError &&on_error) {
        auto self = caf::actor_cast<caf::actor>(this);
        if (rep.kind == SourceFrames) {
            mail(
                media::add_media_source_atom_v, rep.kind, rep.path,
                rep.frame_range, rate)
                .request(self, caf::infinite)
                .then(
                    std::forward<OnSuccess>(on_success),
                    std::forward<OnError>(on_error));
        } else {
            mail(media::add_media_source_atom_v, rep.kind, rep.path, rate, true)
                .request(self, caf::infinite)
                .then(
                    std::forward<OnSuccess>(on_success),
                    std::forward<OnError>(on_error));
        }
    }

    // Common path: given a primary source, create the media actor,
    // add to playlist/subset, set metadata, set viewer, and sequentially
    // add any remaining representations as secondary sources.
    void add_to_playlist(
        const UuidActor &primary_ua,
        const std::string &media_name,
        bool set_viewer,
        caf::actor playlist,
        caf::actor subset,
        const JsonStore &payload,
        const std::vector<Representation> &secondaries,
        const FrameRate &rate) {

        auto media_uuid  = Uuid::generate();
        auto media_actor = spawn<media::MediaActor>(
            media_name, media_uuid, UuidActorVector());

        UuidActorVector src_vec;
        src_vec.push_back(primary_ua);

        mail(media::add_media_source_atom_v, src_vec)
            .request(media_actor, std::chrono::seconds(10))
            .then(
                [=, this](bool) mutable {
                    anon_mail(utility::name_atom_v, media_name).send(media_actor);

                    if (payload.find("metadata") != payload.end()) {
                        anon_mail(
                            json_store::set_json_atom_v, Uuid(),
                            JsonStore(payload["metadata"]),
                            std::string("/shotgrid"))
                            .send(media_actor);
                    }

                    auto media_ua = UuidActor(media_uuid, media_actor);
                    UuidActorVector media_vec;
                    media_vec.push_back(media_ua);

                    mail(playlist::add_media_atom_v, media_vec, Uuid())
                        .request(playlist, std::chrono::seconds(10))
                        .then(
                            [=, this](bool) mutable {
                                if (subset) {
                                    anon_mail(playlist::add_media_atom_v, media_ua, Uuid())
                                        .send(subset);
                                    anon_mail(playlist::select_media_atom_v, UuidList({media_uuid}))
                                        .send(subset);
                                    if (set_viewer) {
                                        set_viewer_to_subset(subset, media_name);
                                    }
                                }
                                spdlog::info(
                                    "RdoMediaLoader: loaded {} (primary source)",
                                    media_name);

                                // Add secondary sources in background, in order.
                                for (const auto &rep : secondaries) {
                                    request_source(
                                        rep, rate,
                                        [=](const UuidActor &sec_ua) mutable {
                                            if (!sec_ua.uuid().is_null()) {
                                                UuidActorVector sec_vec;
                                                sec_vec.push_back(sec_ua);
                                                anon_mail(
                                                    media::add_media_source_atom_v,
                                                    sec_vec)
                                                    .send(media_actor);
                                                spdlog::info(
                                                    "RdoMediaLoader: {} added for {}",
                                                    rep.kind, media_name);
                                            }
                                        },
                                        [=](caf::error &err) {
                                            spdlog::warn(
                                                "RdoMediaLoader: {} secondary: {}",
                                                rep.kind, to_string(err));
                                        });
                                }
                            },
                            [=](caf::error &err) {
                                spdlog::warn(
                                    "RdoMediaLoader: playlist add: {}", to_string(err));
                            });
                },
                [=](caf::error &err) {
                    spdlog::warn("RdoMediaLoader: add source: {}", to_string(err));
                });
    }

    // Set the viewport to the given subset.
    void set_viewer_to_subset(caf::actor subset, const std::string &media_name) {
        auto studio = system().registry().template get<caf::actor>(studio_registry);
        if (!studio) return;
        mail(session::session_atom_v)
            .request(studio, std::chrono::seconds(5))
            .then(
                [=, this](caf::actor session) {
                    mail(utility::uuid_atom_v)
                        .request(subset, std::chrono::seconds(5))
                        .then(
                            [=](const Uuid &subset_uuid) {
                                anon_mail(
                                    session::viewport_active_media_container_atom_v,
                                    subset_uuid)
                                    .send(session);
                                spdlog::info("RdoMediaLoader: set viewer to subset");
                            },
                            [=](caf::error &err) {
                                spdlog::warn(
                                    "RdoMediaLoader: set_viewer UUID failed: {}",
                                    to_string(err));
                            });
                },
                [=](caf::error &err) {
                    spdlog::warn(
                        "RdoMediaLoader: set_viewer session lookup failed: {}",
                        to_string(err));
                });
    }
};


// Plugin — registers in actor system, spawns worker pool.
class RdoMediaLoaderPlugin : public xstudio::plugin::StandardPlugin {
  public:
    RdoMediaLoaderPlugin(
        caf::actor_config &cfg, const utility::JsonStore &init_settings)
        : xstudio::plugin::StandardPlugin(cfg, "RdoMediaLoader", init_settings) {

        system().registry().put(
            RdoMediaLoaderRegistry, caf::actor_cast<caf::actor>(this));
        spdlog::info("RdoMediaLoader: registered as {}", RdoMediaLoaderRegistry);

        // Clear any leftover web downloads from a previous session.
        cleanup_web_cache();

        for (int i = 0; i < 4; ++i) {
            auto w = spawn<MediaLoaderWorker>();
            link_to(w);
            workers_.push_back(w);
        }
    }

    ~RdoMediaLoaderPlugin() override = default;

    caf::message_handler message_handler_extensions() override {
        return {
            [=](playlist::add_media_atom,
                const JsonStore &payload,
                caf::actor playlist,
                caf::actor subset,
                const FrameRate &rate) {
                auto &worker = workers_[next_worker_++ % workers_.size()];
                anon_mail(
                    playlist::add_media_atom_v, payload,
                    std::move(playlist), std::move(subset), rate)
                    .send(worker);
            },
        };
    }

  private:
    std::vector<caf::actor> workers_;
    size_t next_worker_{0};
};

} // anonymous namespace

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<
                plugin_manager::PluginFactoryTemplate<RdoMediaLoaderPlugin>>(
                Uuid("b1c2d3e4-f5a6-7b8c-9d0e-1f2a3b4c5d6e"),
                "RdoMediaLoader",
                plugin_manager::PluginFlags::PF_CUSTOM,
                true,
                "xStudio",
                "Fast media loader for RDO Browser",
                semver::version("1.0.0"))}));
}
}
