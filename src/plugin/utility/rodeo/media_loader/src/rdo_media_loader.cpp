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

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <regex>
#include <spdlog/spdlog.h>

#include "xstudio/atoms.hpp"
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

// Worker actor — follows DNEG's promise-chaining pattern.
// Step 1: create MOV source → Step 2: create EXR source → Step 3: assemble media.
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

            // MOV source creation handler (called by do_load via self-request).
            [=](media::add_media_source_atom,
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
                        spawn<media::MediaSourceActor>("MOV", uri, rate, source_uuid);
                    mail(media::acquire_media_detail_atom_v, rate)
                        .request(source, std::chrono::seconds(30))
                        .then(
                            [=](bool) mutable {
                                rp.deliver(UuidActor(source_uuid, source));
                            },
                            [=](caf::error &err) mutable {
                                spdlog::warn("RdoMediaLoader: MOV detail: {}", to_string(err));
                                rp.deliver(UuidActor(source_uuid, source));
                            });
                } catch (const std::exception &e) {
                    spdlog::warn("RdoMediaLoader: MOV: {}", e.what());
                    rp.deliver(UuidActor());
                }
                return rp;
            },

            // EXR/frames source creation handler.
            [=](media::add_media_source_atom,
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
                                  "EXR", uri, rate, source_uuid)
                            : spawn<media::MediaSourceActor>(
                                  "EXR", uri, frame_list, rate, source_uuid);

                    mail(media::acquire_media_detail_atom_v, rate)
                        .request(source, std::chrono::seconds(30))
                        .then(
                            [=](bool) mutable {
                                rp.deliver(UuidActor(source_uuid, source));
                            },
                            [=](caf::error &err) mutable {
                                spdlog::warn("RdoMediaLoader: EXR detail: {}", to_string(err));
                                rp.deliver(UuidActor(source_uuid, source));
                            });
                } catch (const std::exception &e) {
                    spdlog::warn("RdoMediaLoader: EXR: {}", e.what());
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
        auto frame_range = payload.value("frame_range", std::string());
        auto media_name  = payload.value("version_name", std::string("unknown"));
        auto set_viewer  = payload.value("set_viewer", false);

        if (!movie_path.empty()) {
            // MOV available: load MOV as primary, EXR as secondary.
            mail(media::add_media_source_atom_v, movie_path, rate, true)
                .request(caf::actor_cast<caf::actor>(this), caf::infinite)
                .then(
                    [=, this](const UuidActor &mov_ua) mutable {
                        if (mov_ua.uuid().is_null()) {
                            // MOV creation failed — fall back to EXR-only.
                            if (!frames_path.empty()) {
                                spdlog::info("RdoMediaLoader: MOV failed, falling back to EXR for {}", media_name);
                                load_exr_primary(frames_path, frame_range, media_name, set_viewer, playlist, subset, payload, rate);
                            } else {
                                spdlog::warn("RdoMediaLoader: no sources for {}", media_name);
                            }
                            return;
                        }
                        add_to_playlist(mov_ua, media_name, set_viewer, playlist, subset, payload, frames_path, frame_range, rate);
                    },
                    [=, this](caf::error &err) mutable {
                        spdlog::warn("RdoMediaLoader: MOV step failed: {}", to_string(err));
                        if (!frames_path.empty()) {
                            load_exr_primary(frames_path, frame_range, media_name, set_viewer, playlist, subset, payload, rate);
                        }
                    });
        } else if (!frames_path.empty()) {
            // No MOV: load EXR as primary source.
            load_exr_primary(frames_path, frame_range, media_name, set_viewer, playlist, subset, payload, rate);
        } else {
            spdlog::warn("RdoMediaLoader: no sources for {}", media_name);
        }
    }

    // Load EXR as the primary (and only) source.
    void load_exr_primary(
        const std::string &frames_path,
        const std::string &frame_range,
        const std::string &media_name,
        bool set_viewer,
        caf::actor playlist,
        caf::actor subset,
        const JsonStore &payload,
        const FrameRate &rate) {

        spdlog::info("RdoMediaLoader: loading EXR as primary for {}", media_name);
        mail(media::add_media_source_atom_v, frames_path, frame_range, rate)
            .request(caf::actor_cast<caf::actor>(this), caf::infinite)
            .then(
                [=, this](const UuidActor &exr_ua) mutable {
                    if (exr_ua.uuid().is_null()) {
                        spdlog::warn("RdoMediaLoader: EXR creation failed for {}", media_name);
                        return;
                    }
                    // No secondary source when EXR is primary.
                    add_to_playlist(exr_ua, media_name, set_viewer, playlist, subset, payload, "", "", rate);
                },
                [=](caf::error &err) {
                    spdlog::warn("RdoMediaLoader: EXR primary: {}", to_string(err));
                });
    }

    // Common path: given a primary source, create the media actor,
    // add to playlist/subset, set metadata, set viewer, and optionally
    // add a secondary EXR source.
    void add_to_playlist(
        const UuidActor &primary_ua,
        const std::string &media_name,
        bool set_viewer,
        caf::actor playlist,
        caf::actor subset,
        const JsonStore &payload,
        const std::string &secondary_frames_path,
        const std::string &secondary_frame_range,
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
                                spdlog::info("RdoMediaLoader: loaded {} (primary source)", media_name);

                                // Add secondary EXR source in background.
                                if (!secondary_frames_path.empty()) {
                                    mail(media::add_media_source_atom_v, secondary_frames_path, secondary_frame_range, rate)
                                        .request(caf::actor_cast<caf::actor>(this), caf::infinite)
                                        .then(
                                            [=](const UuidActor &exr_ua) mutable {
                                                if (!exr_ua.uuid().is_null()) {
                                                    UuidActorVector exr_vec;
                                                    exr_vec.push_back(exr_ua);
                                                    anon_mail(media::add_media_source_atom_v, exr_vec)
                                                        .send(media_actor);
                                                    spdlog::info("RdoMediaLoader: EXR added for {}", media_name);
                                                }
                                            },
                                            [=](caf::error &err) {
                                                spdlog::warn("RdoMediaLoader: EXR secondary: {}", to_string(err));
                                            });
                                }
                            },
                            [=](caf::error &err) {
                                spdlog::warn("RdoMediaLoader: playlist add: {}", to_string(err));
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
                                spdlog::warn("RdoMediaLoader: set_viewer UUID failed: {}", to_string(err));
                            });
                },
                [=](caf::error &err) {
                    spdlog::warn("RdoMediaLoader: set_viewer session lookup failed: {}", to_string(err));
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
