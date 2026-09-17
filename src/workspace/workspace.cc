#include "workspace.h"

#include "camera/camera_config.h"
#include "config/config_registry.h"
#include "point_streamer/point_streamer.h"
#include "publishers/message_streamer/message_streamer.h"
#include "publishers/mqtt/mqtt_client.h"
#include "publishers/osc/osc_sender.h"
#include "publishers/workspace_publisher.h"
#include "receivers/osc/osc_receiver.h"
#include "recorder/session_recorder.h"
#include "session/session.h"
#include "session/session_config.h"
#include "workspace_config.h"

#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/Manager.h>

#include <app_settings/app_settings.h>
#include <core/logger/logger.h>
#include <core/uuid/uuid.h>
#include <metrics/prometheus_server.h>
#include <plugins/devices/device_plugin.h>
#include <plugins/devices/device_tree.h>
#include <plugins/devices/device_variants.h>
#include <plugins/plugin_loader.h>
#include <pointcaster/plugin_manager_lock.h>
#include <pointcaster/task_pool.h>

#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#endif

namespace pc {

using namespace Corrade::PluginManager;
using namespace Corrade::Containers;

Workspace::Workspace(const WorkspaceConfiguration &initial) : config(initial) {
  if (config.id.empty()) {
    config.id = pc::uuid::word();
  } else {
    // if an id was already assigned at workspace initialisation time,
    // we loaded it from disk
    auto_loaded_config = true;
  }

  if (config.sessions.empty()) {
    // every workspace needs a session with a camera
    config.sessions.emplace_back(pc::uuid::word(), "session_1",
                                 CameraConfiguration{.id = pc::uuid::word()});
  }

  // find and initialise plugins
  backend_plugin_manager = plugins::load_backend_plugins(*this);
  device_plugin_manager = plugins::load_device_plugins(*this);
  operator_plugin_manager = plugins::load_operator_plugins(*this);

  // instantiate device plugins for the initial config
  sync_devices();

  // instantiate session operator pipelines
  sync_sessions();

  refresh_device_session_membership();

  rebuild_config_registry();

  // initialise workspace instances
  osc_receiver = std::make_unique<receivers::OscReceiver>(*this);
  session_recorder = std::make_unique<recorder::SessionRecorder>(*this);
  point_streamer = std::make_unique<networking::PointStreamer>(*this);

  workspace_publisher = std::make_unique<publishers::WorkspacePublisher>(*this);
  message_streamer = std::make_unique<publishers::MessageStreamer>(*this);
  mqtt_client = std::make_unique<publishers::MqttClient>(*this);
  osc_sender = std::make_unique<publishers::OscSender>(*this);

  // TODO maybe the metrics server shouldn't be a singleton and should
  // follow the same pattern as session_recorder & point_streamer belonging
  // to this workspace class and the injected workspace is what grabs it
  // wherever it's needed... at least just so it can be torn down and restarted
  // at will
  metrics::PrometheusServer::initialise();

  _metrics_thread = std::jthread(
      [this](std::stop_token stop_token) { metrics_thread_work(stop_token); });
}

namespace {
// for teardown tasks that would otherwise block the UI
BS::thread_pool<> &teardown_pool() {
  static BS::thread_pool<> pool{1};
  return pool;
}
} // namespace

Workspace::~Workspace() {
  teardown_pool().wait();
}

// remove devices from the workspace asynchronously
void Workspace::dispose_device(
    Corrade::Containers::Pointer<devices::DevicePlugin> device) {
  if (!device) return;
  device->detach_callbacks();
  teardown_pool().detach_task([device = std::move(device)]() mutable {
    device->shutdown();
    std::scoped_lock plugin_lock(plugin_manager_access());
    device = nullptr;
  });
}

void Workspace::apply_new_config(const WorkspaceConfiguration &new_config,
                                 bool should_sync_devices) {
  {
    std::scoped_lock lock(config_access);
    config = new_config;
  }
  if (should_sync_devices) sync_devices();
  sync_sessions();
  refresh_device_session_membership();
  rebuild_config_registry();
  if (osc_receiver) osc_receiver->reconfigure();
}

namespace {
// operators sit in a vector, which register_config skips over, so each one is
// registered by its own id underneath whichever node owns it
void register_operators(
    ConfigRegistry &registry, const std::string &prefix,
    std::vector<operators::OperatorConfigurationVariant> &operators) {
  for (auto &operator_variant : operators) {
    const auto operator_prefix =
        prefix + "/operators/" + operators::operator_address(operator_variant);
    std::visit(
        [&](auto &operator_config) {
          pc::register_config(registry, operator_prefix, operator_config);
        },
        operator_variant);
  }
}
} // namespace

// TODO not sure about this...
// could probs be comp time registry? idk at least
// it probs doesn't need to be rebuilt often
void Workspace::rebuild_config_registry() {
  config_registry.clear();

  // register configs for session values
  for (auto &session_config : config.sessions) {
    const std::string prefix = "session/" + session_address(session_config);
    pc::register_config(config_registry, prefix, session_config);
    register_operators(config_registry, prefix, session_config.operators);
  }

  // and for other configs held by the workspace instance
  pc::register_config(config_registry, "streaming", config.point_streamer);
  pc::register_config(config_registry, "osc", config.osc_receiver);
  pc::register_config(config_registry, "publishers", config.publishers);

  // register configs for every device plugin
  for (auto &device_plugin : devices) {
    if (!device_plugin) continue;
    std::visit(
        [this](auto &device_config) {
          const std::string prefix =
              "device/" + pc::devices::device_address(config, device_config.id);
          pc::register_config(config_registry, prefix, device_config);
          // groups carry no operators of their own
          if constexpr (requires { device_config.operators; }) {
            register_operators(config_registry, prefix,
                               device_config.operators);
          }
        },
        device_plugin->config_variant());
  }

  // register configs recursively for every device group and set up some
  // callbacks for specific nested config types (nested sequences)

  const auto propagate_to_group_children = [this](
                                               const std::string &group_id,
                                               const std::string &field_path,
                                               const pc::ConfigValue &value) {
    for (auto &device_variant : config.devices) {
      std::visit(
          [&](auto &device_config) {
            if (device_config.parent_id == group_id) {
              const std::string device_address =
                  pc::devices::device_address(config, device_config.id);
              config_registry.set("device/" + device_address + "/" + field_path,
                                  value);
              // For current_frame, call on_config_field_changed directly so
              // the frame loads before the tick can overwrite the config value
              if (field_path == "sequence/current_frame") {
                for (auto &device_plugin : devices) {
                  if (!device_plugin) continue;
                  const auto [did, _] = pc::devices::device_info_from_variant(
                      device_plugin->config_variant());
                  if (std::string(did) == device_config.id) {
                    device_plugin->on_config_field_changed(field_path);
                    break;
                  }
                }
              }
            }
          },
          device_variant);
    }
    for (auto &child_group : config.device_groups) {
      if (child_group.parent_id == group_id) {
        const std::string child_group_address =
            pc::devices::device_address(config, child_group.id);
        config_registry.set("device/" + child_group_address + "/" + field_path,
                            value);
      }
    }
  };

  for (auto &group_config : config.device_groups) {
    const std::string group_address =
        pc::devices::device_address(config, group_config.id);
    pc::register_config(config_registry, "device/" + group_address,
                        group_config);

    const std::string base = "device/" + group_address + "/sequence/";

    config_registry.register_field(
        base + "playing",
        {.get = [&group_config]() -> pc::ConfigValue {
           return group_config.sequence.playing;
         },
         .set =
             [&group_config,
              propagate_to_group_children](pc::ConfigValue value) {
               group_config.sequence.playing =
                   pc::from_config_value<bool>(value);
               propagate_to_group_children(group_config.id, "sequence/playing",
                                           value);
             }});

    config_registry.register_field(
        base + "looping",
        {.get = [&group_config]() -> pc::ConfigValue {
           return group_config.sequence.looping;
         },
         .set =
             [&group_config,
              propagate_to_group_children](pc::ConfigValue value) {
               group_config.sequence.looping =
                   pc::from_config_value<bool>(value);
               propagate_to_group_children(group_config.id, "sequence/looping",
                                           value);
             }});

    config_registry.register_field(
        base + "current_frame",
        {.get = [&group_config]() -> pc::ConfigValue {
           return group_config.sequence.current_frame;
         },
         .set =
             [&group_config,
              propagate_to_group_children](pc::ConfigValue value) {
               group_config.sequence.current_frame =
                   pc::from_config_value<int>(value);
               propagate_to_group_children(group_config.id,
                                           "sequence/current_frame", value);
             }});
  }
}

void Workspace::sync_sessions() {
  pc::logger()->trace("Syncing sessions");

  std::vector<SessionConfiguration> session_configs;
  {
    std::scoped_lock lock(config_access);
    session_configs = config.sessions;
  }

  std::vector<std::unique_ptr<Session>> deleted_sessions;
  {
    std::scoped_lock lock(sessions_access);

    for (auto &session_config : session_configs) {
      auto it = sessions.find(session_config.id);
      if (it == sessions.end()) {
        // create any new sessions
        sessions.emplace(session_config.id,
                         std::make_unique<Session>(*this, session_config));
      } else if (it->second) {
        // push updated config into existing sessions
        it->second->update_config(session_config);
      }
    }

    for (auto it = sessions.begin(); it != sessions.end();) {
      const auto session_config = std::ranges::find(session_configs, it->first,
                                                    &SessionConfiguration::id);
      if (it->second && session_config != session_configs.end()) {
        it++;
        continue;
      }
      // extra sessions that aren't in the config must have been deleted
      deleted_sessions.push_back(std::move(it->second));
      it = sessions.erase(it);
    }
  }

  for (auto &session : deleted_sessions) {
    std::scoped_lock plugin_lock(plugin_manager_access());
    session.reset();
  }
}

void Workspace::refresh_device_session_membership() {
  std::vector<std::pair<devices::DevicePlugin *, bool>> changed_devices;
  {
    std::scoped_lock lock(config_access);
    for (auto &device : devices) {
      if (!device) continue;
      bool in_any_session = false;
      std::visit(
          [&](auto &device_config) {
            in_any_session = devices::in_any_session(config, device_config.id);
          },
          device->config_variant());
      if (in_any_session == device->in_any_session()) continue;
      device->set_in_any_session(in_any_session);
      changed_devices.emplace_back(device.get(), in_any_session);
    }
  }
  for (auto &[device, included] : changed_devices) {
    device->on_session_membership_changed(included);
  }
}

void Workspace::sync_devices() {
  pc::logger()->trace("Syncing devices");
  std::vector<devices::DeviceConfigurationVariant> device_configs;
  {
    std::scoped_lock lock(config_access);
    device_configs = config.devices;
  }

  // check for duplicates
  std::unordered_map<std::string, std::size_t> existing_index_by_id;
  existing_index_by_id.reserve(devices.size());

  for (std::size_t i = 0; i < devices.size(); ++i) {
    auto &p = devices[i];
    if (!p) continue;

    const auto [id, _] = device_info_from_variant(p->config_variant());
    if (id.empty()) continue;

    // If duplicates exist, keep the first
    if (!existing_index_by_id.contains(id)) {
      existing_index_by_id.emplace(id, i);
    }
  }

  // the result collection of device plugins
  std::vector<Pointer<pc::devices::DevicePlugin>> new_devices;
  new_devices.reserve(device_configs.size());

  for (auto &device_variant : device_configs) {

    auto [device_id, device_plugin_name] =
        device_info_from_variant(device_variant);

    if (device_id.empty()) {
      pc::logger()->warn("Device config missing id; skipping device entry");
      continue;
    }

    bool plugin_loaded =
        plugins::is_loaded(*device_plugin_manager, device_plugin_name);

    if (!plugin_loaded) {
      pc::logger()->error(
          "Device plugin '{}' not loaded when syncing device '{}'",
          device_plugin_name, device_id);
      // treat the device plugin as NullDevice
      device_plugin_name = "NullDevice";
      plugin_loaded = true;
    }

    Pointer<pc::devices::DevicePlugin> device_plugin = nullptr;

    auto it = existing_index_by_id.find(device_id);
    if (it != existing_index_by_id.end()) {
      // Reuse existing plugin instance for this id if variant type matches.
      const std::size_t existing_index = it->second;
      existing_index_by_id.erase(it);

      if (existing_index < devices.size() && devices[existing_index]) {
        auto &existing_ptr = devices[existing_index];
        auto *existing_plugin = existing_ptr.get();

        const auto [_, existing_plugin_name] =
            device_info_from_variant(existing_plugin->config_variant());

        if (existing_plugin_name == device_plugin_name) {
          // Update config in-place (does not restart pipelines by itself).
          existing_plugin->update_config(device_variant);
          device_plugin = std::move(existing_ptr);
        } else {
          // Same id but different variant type (device type changed), so
          // replace plugin instance
          pc::logger()->info("Device id='{}' changed plugin type '{}' -> '{}'",
                             device_id, existing_plugin_name,
                             device_plugin_name);

          dispose_device(std::move(existing_ptr));
          bool new_device_instance = false;

          {
            std::scoped_lock plugin_lock(plugin_manager_access());
            if (plugin_loaded) {
              device_plugin =
                  device_plugin_manager->instantiate(device_plugin_name);
              new_device_instance = true;
            } else {
              device_plugin = device_plugin_manager->instantiate("NullDevice");
              new_device_instance = true;
            }
          }
          device_plugin->set_is_discovery_instance(false);
          if (new_device_instance) device_plugin->init(*this);
          device_plugin->update_config(device_variant);
        }
      }
    } else {
      if (plugin_loaded) {
        pc::logger()->trace("Instantiating a new device plugin instance");
        std::scoped_lock plugin_lock(plugin_manager_access());
        device_plugin = device_plugin_manager->instantiate(device_plugin_name);
      }
      if (device_plugin) {
        std::visit(
            [](auto &&config) {
              pc::logger()->trace("Applying device configuration: {}",
                                  config.id);
            },
            device_variant);
        device_plugin->set_is_discovery_instance(false);
        device_plugin->init(*this);
        device_plugin->update_config(device_variant);
      }
    }
    new_devices.push_back(std::move(device_plugin));
  }

  for (const auto &[id, idx] : existing_index_by_id) {
    if (idx >= devices.size()) continue;
    if (!devices[idx]) continue;
    pc::logger()->trace("Removing device plugin id='{}'", id);
    dispose_device(std::move(devices[idx]));
  }

  devices = std::move(new_devices);
}

} // namespace pc
