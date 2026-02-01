// SPDX-License-Identifier: Apache-2.0
/**
 * Rodeo ShotGrid Data Source Bridge
 *
 * A minimal C++ data source that bridges to the Python RdoShotGridDataSource plugin.
 * Registers as "RDOSHOTGRID" in the actor registry so C++ plugins can query it
 * using the standard data source pattern (get_data_atom_v).
 *
 * Communication flow:
 * 1. C++ plugin sends get_data_atom_v with BatchQueryVersions request
 * 2. This bridge finds the Python plugin via plugin manager
 * 3. Sets Python plugin's "SG Request" attribute
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

// Name of the Python plugin we delegate to
const std::string python_plugin_name{"RdoShotGridDataSource"};

// Timeout for waiting for Python response
constexpr auto PYTHON_RESPONSE_TIMEOUT = 30s;

}  // namespace

/**
 * RdoShotGridDataSource - C++ bridge to Python ShotGrid data source.
 */
class RdoShotGridDataSource : public caf::event_based_actor {
  public:
    RdoShotGridDataSource(caf::actor_config &cfg)
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
     * This finds the Python RdoShotGridDataSource plugin, sets its
     * "SG Request" attribute with the request, and polls "SG Response"
     * for the result.
     */
    void delegate_to_python(
        caf::typed_response_promise<JsonStore> rp,
        const JsonStore &request) {

        try {
            // Find the Python plugin via plugin manager
            auto plugin_manager =
                system().registry().template get<caf::actor>(plugin_manager_registry);

            if (!plugin_manager) {
                throw std::runtime_error("Plugin manager not found");
            }

            scoped_actor sys{system()};

            // Get list of plugins to find our Python plugin
            auto plugin_details = request_receive<std::vector<plugin_manager::PluginDetail>>(
                *sys, plugin_manager, utility::detail_atom_v);

            caf::actor python_plugin;
            Uuid python_plugin_uuid;

            for (const auto &detail : plugin_details) {
                if (detail.name_ == python_plugin_name && detail.enabled_) {
                    python_plugin_uuid = detail.uuid_;
                    break;
                }
            }

            if (python_plugin_uuid.is_null()) {
                throw std::runtime_error(
                    "Python plugin '" + python_plugin_name + "' not found or not enabled");
            }

            // Get the plugin actor
            python_plugin = request_receive<caf::actor>(
                *sys, plugin_manager, plugin_manager::spawn_plugin_atom_v, python_plugin_uuid);

            if (!python_plugin) {
                throw std::runtime_error("Could not spawn Python plugin");
            }

            // Set the request attribute on the Python plugin
            auto request_json = request.dump();
            spdlog::info("RdoShotGridDataSource: Sending request to Python: {} chars",
                request_json.size());

            // Find and set the "SG Request" attribute
            // This triggers the Python plugin to process the request
            request_receive<bool>(
                *sys,
                python_plugin,
                module::set_attribute_value_atom_v,
                std::string("SG Request"),
                request_json);

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
                    auto response_str = request_receive<std::string>(
                        *sys,
                        python_plugin,
                        module::get_attribute_value_atom_v,
                        std::string("SG Response"));

                    if (!response_str.empty()) {
                        response = JsonStore(nlohmann::json::parse(response_str));

                        // Clear the response attribute for next request
                        request_receive<bool>(
                            *sys,
                            python_plugin,
                            module::set_attribute_value_atom_v,
                            std::string("SG Response"),
                            std::string(""));

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
