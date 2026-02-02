// SPDX-License-Identifier: Apache-2.0
/**
 * Rodeo ShotGrid Data Source Bridge
 *
 * A minimal C++ data source that bridges to the Python RdoShotGridDataSource plugin.
 * Registers as "RDOSHOTGRID" in the actor registry so C++ plugins can query it
 * using the standard data source pattern (get_data_atom_v).
 *
 * Communication flow:
 * 1. Python plugin starts and registers itself with this bridge via join_broadcast_atom
 * 2. C++ plugin sends get_data_atom_v with BatchQueryVersions request
 * 3. This bridge sets Python plugin's "SG Request" attribute
 * 4. Polls Python plugin's "SG Response" attribute for result
 * 5. Returns result to calling C++ plugin
 */

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <chrono>
#include <thread>

#include "xstudio/atoms.hpp"
#include "xstudio/data_source/data_source.hpp"
#include "xstudio/utility/helpers.hpp"
#include "xstudio/utility/json_store.hpp"
#include "xstudio/module/module.hpp"
#include "xstudio/plugin_manager/plugin_manager.hpp"

using namespace xstudio;
using namespace xstudio::utility;
using namespace std::chrono_literals;

namespace {

// Registry name for this data source
const std::string rdo_shotgrid_registry{"RDOSHOTGRID"};

// Timeout for waiting for Python response
constexpr auto PYTHON_RESPONSE_TIMEOUT = 30s;

}  // namespace

/**
 * RdoShotGridDataSource - C++ bridge to Python ShotGrid data source.
 */
class RdoShotGridDataSource : public caf::event_based_actor {
  public:
    RdoShotGridDataSource(caf::actor_config &cfg, const JsonStore & = JsonStore())
        : caf::event_based_actor(cfg) {
        spdlog::info("RdoShotGridDataSource bridge created");
        utility::print_on_exit(this, "RdoShotGridDataSource");

        // Register in CAF actor registry so other plugins can find us
        system().registry().put(rdo_shotgrid_registry, caf::actor_cast<caf::actor>(this));

        behavior_.assign(
            [=](xstudio::broadcast::broadcast_down_atom, const caf::actor_addr &) {},

            [=](utility::name_atom) -> std::string {
                return "RdoShotGridDataSource";
            },

            // Python plugin registers itself with us via join_broadcast_atom
            // This is called by the Python RdoShotGridDataSource when it starts
            [=](broadcast::join_broadcast_atom, caf::actor python_actor) mutable {
                spdlog::info("RdoShotGridDataSource: Python plugin registered");
                python_plugin_ = python_actor;
            },

            // Handle get_data requests - this is the main entry point
            [=](data_source::get_data_atom, const JsonStore &request) -> result<JsonStore> {
                auto rp = make_response_promise<JsonStore>();
                handle_get_data(rp, request);
                return rp;
            },

            // Unused data source methods - return empty/error
            [=](data_source::use_data_atom, const caf::uri &) -> UuidActorVector {
                return UuidActorVector();
            },

            [=](data_source::use_data_atom,
                const caf::actor &,
                const FrameRate &) -> result<UuidActorVector> {
                return UuidActorVector();
            },

            [=](data_source::use_data_atom, const JsonStore &) -> result<JsonStore> {
                return make_error(xstudio_error::error, "use_data not supported");
            },

            [=](data_source::put_data_atom, const JsonStore &) -> result<JsonStore> {
                return make_error(xstudio_error::error, "put_data not supported");
            },

            [=](data_source::post_data_atom, const JsonStore &) -> result<JsonStore> {
                return make_error(xstudio_error::error, "post_data not supported");
            });
    }

    ~RdoShotGridDataSource() override {
        system().registry().erase(rdo_shotgrid_registry);
    }

    caf::behavior make_behavior() override { return behavior_; }

  private:
    /**
     * Handle get_data request by delegating to Python plugin.
     */
    void handle_get_data(
        caf::typed_response_promise<JsonStore> rp,
        const JsonStore &request) {

        try {
            auto operation = request.value("operation", std::string());

            if (operation == "BatchQueryVersions") {
                // Delegate to Python plugin
                delegate_to_python(rp, request);
            } else {
                spdlog::warn("RdoShotGridDataSource: Unknown operation '{}'", operation);
                rp.deliver(make_error(xstudio_error::error,
                    "Unknown operation: " + operation));
            }
        } catch (const std::exception &err) {
            spdlog::warn("RdoShotGridDataSource: Error handling request: {}", err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

    /**
     * Delegate request to Python plugin via module attributes.
     *
     * Uses the registered Python plugin actor (set via join_broadcast_atom),
     * sets its "SG Request" attribute with the request, and polls
     * "SG Response" for the result.
     */
    void delegate_to_python(
        caf::typed_response_promise<JsonStore> rp,
        const JsonStore &request) {

        try {
            // Check if Python plugin has registered
            if (!python_plugin_) {
                throw std::runtime_error(
                    "Python RdoShotGridDataSource not registered. "
                    "Ensure the Python plugin is loaded and has registered via join_broadcast_atom.");
            }

            scoped_actor sys{system()};

            // Set the request attribute on the Python plugin
            auto request_json = request.dump();
            spdlog::info("RdoShotGridDataSource: Sending request to Python: {} chars",
                request_json.size());

            // Find and set the "SG Request" attribute
            // This triggers the Python plugin to process the request
            request_receive<bool>(
                *sys,
                python_plugin_,
                module::change_attribute_value_atom_v,
                std::string("SG Request"),
                JsonStore(request_json),
                true);

            // Poll for response on "SG Response" attribute
            auto start_time = std::chrono::steady_clock::now();
            auto response = JsonStore();

            while (true) {
                std::this_thread::sleep_for(50ms);

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed > PYTHON_RESPONSE_TIMEOUT) {
                    throw std::runtime_error("Timeout waiting for Python response");
                }

                // Try to get the response attribute
                try {
                    auto response_json = request_receive<JsonStore>(
                        *sys,
                        python_plugin_,
                        module::attribute_value_atom_v,
                        std::string("SG Response"));

                    auto response_str = response_json.get<std::string>();
                    if (!response_str.empty()) {
                        response = JsonStore(nlohmann::json::parse(response_str));

                        // Clear the response attribute for next request
                        request_receive<bool>(
                            *sys,
                            python_plugin_,
                            module::change_attribute_value_atom_v,
                            std::string("SG Response"),
                            JsonStore(""),
                            true);

                        break;
                    }
                } catch (...) {
                    // Attribute might not exist yet, continue polling
                }
            }

            spdlog::info("RdoShotGridDataSource: Got response from Python");
            rp.deliver(response);

        } catch (const std::exception &err) {
            spdlog::warn("RdoShotGridDataSource: Error delegating to Python: {}", err.what());
            rp.deliver(make_error(xstudio_error::error, err.what()));
        }
    }

  private:
    caf::behavior behavior_;
    caf::actor python_plugin_;  // Registered Python plugin actor
};

extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<
                data_source::DataSourcePlugin<RdoShotGridDataSource>>(
                Uuid("c9d0e1f2-a3b4-5c6d-8e9f-0a1b2c3d4e5f"),
                "RdoShotGrid",
                "xStudio",  // Use "xStudio" as author to auto-enable
                "Rodeo ShotGrid Data Source Bridge",
                semver::version("1.0.0"))}));
}
}
