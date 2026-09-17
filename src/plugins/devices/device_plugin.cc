#include "device_plugin.h"

#include <Corrade/Containers/StringStlView.h>
#include <logger/logger.h>
#include <memory>
#include <pipeline/concurrent_operator_pipeline.h>
#include <plugins/backend/cpu/cpu_backend.h>
#include <plugins/operators/operator_plugin.h>
#include <plugins/operators/operator_variants.h>
#include <unordered_map>
#include <vector>
#include <workspace/workspace.h>

namespace pc::devices {

void DevicePlugin::init(Workspace &workspace) {
  OperatorHost::init(workspace);
  pc::logger()->debug("DevicePlugin::init this={}", fmt::ptr(this));

  if (workspace.backend_plugin_manager) {
    _backends.instantiate(*workspace.backend_plugin_manager,
                          std::string_view{plugin()});
  } else {
    pc::logger()->error("{} initialised without any backend plugins",
                        std::string_view{plugin()});
  }

  // the parameterless init is what implementations override
  init();
}

BackendType DevicePlugin::current_backend_type() const {

  // TODO
  // device plugin current backend stuff is kinda weird,
  // because the selected backend type lives inside its transform config...

  const auto requested_backend = std::visit(
      [](const auto &device_config) {
        if constexpr (requires { device_config.transform; }) {
          return device_config.transform.value().backend.value();
        } else {
          return BackendType::CPU;
        }
      },
      _config);

  const auto resolved_backend = _backends.resolve(requested_backend);
  if (!resolved_backend) return requested_backend;

  if (*resolved_backend != requested_backend) {
    pc::logger()->warn("{} backend is not loaded for device '{}', using {}",
                       backend::backend_name(requested_backend),
                       device_id_from_variant(_config),
                       backend::backend_name(*resolved_backend));
  }

  return *resolved_backend;
}

backend::BackendPlugin *DevicePlugin::current_backend() const {
  return _backends.get(current_backend_type());
}

backend::BackendPlugin *
DevicePlugin::backend_for(BackendType backend_type) const {
  return _backends.get(backend_type);
}

backend::BackendPlugin *DevicePlugin::cpu_backend() const {
  return _backends.cpu();
}

void DevicePlugin::update_config(const DeviceConfigurationVariant &config) {
  _config = config;
  std::visit(
      [this](auto &device_config) {
        if constexpr (requires { device_config.operators; }) {
          sync_operators(device_config.operators);
        }
      },
      _config);

  if (!_workspace) return;

  // we updated the plugin's config copy, now update the actual config thats
  // part of the workspace
  const auto device_id = device_id_from_variant(_config);
  std::string node_prefix;
  {
    std::scoped_lock lock(_workspace->config_access);
    for (auto &device_config_variant : _workspace->config.devices) {
      if (device_id_from_variant(device_config_variant) != device_id) continue;
      device_config_variant = _config;
      node_prefix = "device/" +
                    device_address(_workspace->config, std::string(device_id));
      break;
    }
  }
  if (node_prefix.empty()) return;

  _workspace->config_registry.notify(node_prefix + "/");
}

void DevicePlugin::on_config_field_changed(std::string_view path) {
  // For operator changes, forward the new config to all pipeline worker
  // instances
  if (path.contains("operator")) {
    std::visit(
        [this](const auto &device_config) {
          if constexpr (requires { device_config.operators; }) {
            for (size_t i = 0; i < device_config.operators.size(); i++) {
              const auto &operator_config = device_config.operators[i];
              for (const auto &worker_chain : _pipeline->worker_chains()) {
                worker_chain[i]->update_config(operator_config);
              }
            }
          }
        },
        _config);
  }
}

} // namespace pc::devices