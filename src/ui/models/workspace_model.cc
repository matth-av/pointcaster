#include "workspace_model.h"

#include "app_settings/app_settings.h"
#include "camera_image_provider.h"
#include "layout_saver.h"
#include "models/device_adapter.h"
#include "models/session_recorder_model.h"
#include "models/settings_page_registry.h"
#include "plugins/devices/ply/ply_device_config.h"
#include "plugins/operators/operator_variants.h"
#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuaternion>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QUndoCommand>
#include <QVariant>
#include <QVector3D>
#include <algorithm>
#include <chrono>
#include <config/config_labels.h>
#include <config/transform_config.h>
#include <core/logger/logger.h>
#include <core/util/geometry_utils.h>
#include <core/util/string_utils.h>
#include <core/uuid/uuid.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <plugins/devices/device_group_config.h>
#include <plugins/devices/device_tree.h>
#include <plugins/devices/device_variants.h>
#include <ranges>
#include <session/session.h>
#include <session/session_config.h>
#include <session/session_config_adapter.gen.h>
#include <set>
#include <spdlog/common.h>
#include <string>
#include <string_view>
#include <thread>
#include <ui/layout_saver.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <workspace/workspace.h>

#include <plugins/devices/device_group_config_adapter.gen.h>
// TODO these absolutely need to be polymorphic,
// no way they can be defined at this compile stage
#include <plugins/devices/orbbec/orbbec_device_adapter.gen.h>
#include <plugins/devices/orbbec/orbbec_device_config.h>
#include <plugins/devices/ply/ply_device_adapter.gen.h>
#include <plugins/devices/ply/ply_device_config.h>
#include <plugins/operators/cluster_extraction/cluster_extraction_config_adapter.gen.h>
#include <plugins/operators/fringe_removal/fringe_removal_config_adapter.gen.h>
#include <plugins/operators/range_filter/range_filter_config_adapter.gen.h>

namespace pc::ui {

using pc::SessionConfiguration;
using pc::SessionConfigurationAdapter;

using pc::devices::OrbbecDeviceConfiguration;
using pc::devices::PlyDeviceConfiguration;

namespace {

// -------- helpers --------

static ConfigAdapter *make_operator_config_adapter(
    pc::operators::OperatorConfigurationVariant &config_variant,
    QObject *parent) {
  ConfigAdapter *adapter = nullptr;
  // TODO ok this really isnt right...
  // this can't be static types -- we need ability to instantiate operator
  // adapters without knowing their dynamic lib adapter type
  // ... more like dynamic device plugins?
  std::visit(
      [&adapter, parent](auto &config) {
        using ConfigType = std::decay_t<decltype(config)>;
        if constexpr (std::same_as<ConfigType,
                                   pc::operators::FringeRemovalConfiguration>) {
          adapter = new pc::operators::FringeRemovalConfigurationAdapter(
              config, parent);
        } else if constexpr (std::same_as<ConfigType,
                                          pc::operators::
                                              ClusterExtractionConfiguration>) {
          adapter = new pc::operators::ClusterExtractionConfigurationAdapter(
              config, parent);
        } else if constexpr (std::same_as<
                                 ConfigType,
                                 pc::operators::RangeFilterConfiguration>) {
          adapter = new pc::operators::RangeFilterConfigurationAdapter(config,
                                                                       parent);
        }
      },
      config_variant);
  return adapter;
}

static void subscribe_adapter_to_registry(WorkspaceModel &model,
                                          ConfigAdapter *adapter,
                                          const std::string &prefix) {
  model.unsubscribeFromRegistry(prefix);
  auto adapterPtr = QPointer<ConfigAdapter>(adapter);
  model.subscribeToRegistry(prefix, [adapterPtr,
                                     prefix](std::string_view path) {
    if (!adapterPtr) return;
    if (path.size() < prefix.size()) return;
    const QString qpath =
        QString::fromStdString(std::string(path.substr(prefix.size())));
    QMetaObject::invokeMethod(
        adapterPtr.data(),
        [adapterPtr, qpath]() {
          if (adapterPtr) adapterPtr->notifyFieldChanged(qpath);
        },
        Qt::QueuedConnection);
  });
}

static void resubscribe_operator_adapters(WorkspaceModel &model,
                                          const QList<OperatorAdapter *> &ops) {
  for (auto *opAdapter : ops) {
    auto *opConfigAdapter = opAdapter ? opAdapter->configAdapter() : nullptr;
    if (!opConfigAdapter) continue;
    const std::string operator_prefix =
        opConfigAdapter->configPath().toStdString();
    if (operator_prefix.empty()) continue;
    subscribe_adapter_to_registry(model, opConfigAdapter,
                                  operator_prefix + "/");
  }
}

static int find_session_index_by_id(const pc::WorkspaceConfiguration &config,
                                    const std::string &session_id) {
  for (int i = 0; i < int(config.sessions.size()); ++i) {
    if (config.sessions[size_t(i)].id == session_id) return i;
  }
  return -1;
}

static int find_device_index_by_id(const pc::WorkspaceConfiguration &config,
                                   const std::string &device_id) {
  for (int i = 0; i < int(config.devices.size()); ++i) {
    const auto &variant = config.devices[size_t(i)];
    const bool match =
        std::visit([&](const auto &d) { return d.id == device_id; }, variant);
    if (match) return i;
  }
  return -1;
}

static int
find_device_group_index_by_id(const pc::WorkspaceConfiguration &config,
                              const std::string &group_id) {
  for (int i = 0; i < int(config.device_groups.size()); ++i) {
    if (config.device_groups[size_t(i)].id == group_id) return i;
  }
  return -1;
}

static int find_operator_index_by_id(
    const pc::devices::DeviceConfigurationVariant &device_config,
    const std::string &operator_id) {
  return std::visit(
      [&](const auto &config) -> int {
        if constexpr (requires { config.operators; }) {
          for (int i = 0; i < int(config.operators.size()); ++i) {
            auto [id, pn] =
                pc::operators::operator_info_from_variant(config.operators[i]);
            if (id == operator_id) return i;
          }
        }
        return -1;
      },
      device_config);
}

static int find_session_operator_index_by_id(const pc::SessionConfiguration &s,
                                             const std::string &operator_id) {
  for (int i = 0; i < int(s.operators.size()); ++i) {
    auto [id, pn] =
        pc::operators::operator_info_from_variant(s.operators[size_t(i)]);
    if (id == operator_id) return i;
  }
  return -1;
}

static std::string operator_id_of(pc::operators::OperatorPlugin *plugin) {
  if (!plugin) return {};
  auto [id, plugin_name] =
      pc::operators::operator_info_from_variant(plugin->config_variant());
  return std::string(id);
}

static pc::operators::OperatorConfigurationVariant *
find_device_operator_config(pc::WorkspaceConfiguration &config,
                            const std::string &device_id,
                            const std::string &operator_id) {
  const int device_index = find_device_index_by_id(config, device_id);
  if (device_index < 0 || device_index >= int(config.devices.size()))
    return nullptr;
  auto &device_variant = config.devices[size_t(device_index)];
  const int op_idx = find_operator_index_by_id(device_variant, operator_id);
  if (op_idx < 0) return nullptr;
  return std::visit(
      [op_idx](auto &device_config)
          -> pc::operators::OperatorConfigurationVariant * {
        if constexpr (requires { device_config.operators; }) {
          return &device_config.operators[size_t(op_idx)];
        } else {
          return nullptr;
        }
      },
      device_variant);
}

static pc::operators::OperatorConfigurationVariant *
find_session_operator_config(pc::WorkspaceConfiguration &config,
                             const std::string &session_id,
                             const std::string &operator_id) {
  const int s = find_session_index_by_id(config, session_id);
  if (s < 0 || s >= int(config.sessions.size())) return nullptr;
  auto &session = config.sessions[size_t(s)];
  const int o = find_session_operator_index_by_id(session, operator_id);
  if (o < 0) return nullptr;
  return &session.operators[size_t(o)];
}

// a preview session is keyed by its owner and its operator together
static QString operator_preview_id(const QString &owner_id,
                                   const std::string &operator_id) {
  return owner_id + QStringLiteral("/") + QString::fromStdString(operator_id);
}

static std::optional<pc::operators::OperatorConfigurationVariant>
publish_device_operator_edit(pc::Workspace &workspace,
                             OperatorAdapter *opAdapter,
                             const std::string &device_id,
                             const std::string &operator_id,
                             const QString &changed_path) {
  pc::operators::OperatorConfigurationVariant config;
  {
    std::scoped_lock lock(workspace.config_access);
    auto *storage =
        find_device_operator_config(workspace.config, device_id, operator_id);
    if (!storage) return std::nullopt;
    config = *storage;

    const int device_index =
        find_device_index_by_id(workspace.config, device_id);
    if (device_index >= 0 && device_index < int(workspace.devices.size()) &&
        workspace.devices[size_t(device_index)]) {
      auto &device_config_variant =
          workspace.devices[size_t(device_index)]->config_variant();
      const int op_index =
          find_operator_index_by_id(device_config_variant, operator_id);
      if (op_index >= 0) {
        std::visit(
            [op_index, &config](auto &device_config) {
              if constexpr (requires { device_config.operators; }) {
                device_config.operators[size_t(op_index)] = config;
              }
            },
            device_config_variant);
      }
    }
  }

  if (auto *plugin = opAdapter->plugin()) plugin->update_config(config);
  if (auto *host = opAdapter->host()) {
    host->update_operator_in_pipeline(config, changed_path.toStdString());
    host->reprocess();
  }
  return config;
}

static void publish_session_operator_edit(pc::Workspace &workspace,
                                          OperatorAdapter *opAdapter,
                                          const std::string &session_id,
                                          const std::string &operator_id,
                                          const QString &changed_path) {
  pc::operators::OperatorConfigurationVariant config;
  {
    std::scoped_lock lock(workspace.config_access);
    auto *storage =
        find_session_operator_config(workspace.config, session_id, operator_id);
    if (!storage) return;
    config = *storage;
  }

  if (auto *plugin = opAdapter->plugin()) plugin->update_config(config);
  if (auto *host = opAdapter->host()) {
    host->update_operator_in_pipeline(config, changed_path.toStdString());
    host->reprocess();
  }
}

// compares current adapter list to plugin operators by id & order
static bool operator_structure_changed(DeviceAdapter *adapter,
                                       pc::devices::DevicePlugin *plugin) {
  const auto &current = adapter->operatorAdapters();
  const auto &target = plugin->operators;
  if (int(target.size()) != current.size()) return true;
  for (int i = 0; i < current.size(); ++i) {
    auto *op = current[i];
    if (!op || !op->plugin() || !target[i]) return true;
    auto [a_id, _] = pc::operators::operator_info_from_variant(
        op->plugin()->config_variant());
    auto [b_id, __] =
        pc::operators::operator_info_from_variant(target[i]->config_variant());
    if (a_id != b_id) return true;
  }
  return false;
}

// the address a config adapter would bind to for this operator, matching what
// ConfigAdapter::configStorage() reports
static const void *operator_config_storage_address(
    const pc::operators::OperatorConfigurationVariant &variant) {
  return std::visit([](const auto &config) -> const void * { return &config; },
                    variant);
}

// operator config adapters hold references into the WorkspaceConfiguration,
// so a reused adapter has to be rebound whenever its storage has moved
template <typename ResolveStorage>
static bool operator_config_storage_moved(const QList<OperatorAdapter *> &ops,
                                          ResolveStorage &&resolve_storage) {
  for (auto *opAdapter : ops) {
    if (!opAdapter) return true;
    auto *configAdapter = opAdapter->configAdapter();
    if (!configAdapter) continue;
    const std::string operator_id = operator_id_of(opAdapter->plugin());
    if (operator_id.empty()) continue;
    const auto *storage = resolve_storage(operator_id);
    if (!storage) return true;
    if (configAdapter->configStorage() !=
        operator_config_storage_address(*storage))
      return true;
  }
  return false;
}

// session analogue of operator_structure_changed
static bool
session_operator_structure_changed(const QList<OperatorAdapter *> &current,
                                   pc::Session *session) {
  if (!session) return !current.isEmpty();
  const auto &target = session->operators;
  if (int(target.size()) != current.size()) return true;
  for (int i = 0; i < current.size(); ++i) {
    auto *op = current[i];
    if (!op || !op->plugin() || !target[size_t(i)]) return true;
    auto [a_id, _] = pc::operators::operator_info_from_variant(
        op->plugin()->config_variant());
    auto [b_id, __] = pc::operators::operator_info_from_variant(
        target[size_t(i)]->config_variant());
    if (a_id != b_id) return true;
  }
  return false;
}

bool path_under_prefix(const std::string &path, const std::string &prefix) {
  return path.size() > prefix.size() && path.starts_with(prefix) &&
         path[prefix.size()] == '/';
}

std::set<std::string> reroot_paths(const std::set<std::string> &paths,
                                   const std::string &previous_prefix,
                                   const std::string &new_prefix) {
  std::set<std::string> rerooted;
  for (const auto &path : paths) {
    if (path_under_prefix(path, previous_prefix)) {
      rerooted.insert(new_prefix + path.substr(previous_prefix.size()));
    } else {
      rerooted.insert(path);
    }
  }
  return rerooted;
}

// the config registry prefix a device or group's fields live under
std::string node_path_prefix(const pc::WorkspaceConfiguration &config,
                             const std::string &node_id) {
  return "device/" + pc::devices::device_address(config, node_id);
}

// the device or group a "device/..." path sits under
std::string render_path_node_id(const pc::WorkspaceConfiguration &config,
                                const std::string &path) {
  std::string owner_node_id;
  std::size_t owner_prefix_length = 0;
  const auto consider = [&](const std::string &candidate_node_id) {
    const std::string candidate_prefix =
        node_path_prefix(config, candidate_node_id) + "/";
    if (candidate_prefix.size() <= owner_prefix_length ||
        !path.starts_with(candidate_prefix))
      return;
    owner_prefix_length = candidate_prefix.size();
    owner_node_id = candidate_node_id;
  };
  for (const auto &device_variant : config.devices) {
    consider(std::visit([](const auto &device_config)
                            -> const std::string & { return device_config.id; },
                        device_variant));
  }
  for (const auto &group_config : config.device_groups)
    consider(group_config.id);
  return owner_node_id;
}

// same for a session
std::string session_path_prefix(const pc::WorkspaceConfiguration &config,
                                const std::string &session_id) {
  const int index = find_session_index_by_id(config, session_id);
  if (index < 0) return "session/" + session_id;
  return "session/" + pc::session_address(config.sessions[size_t(index)]);
}

std::string operator_path_prefix(
    const std::string &owner_prefix,
    const pc::operators::OperatorConfigurationVariant &operator_variant) {
  return owner_prefix + "/operators/" +
         pc::operators::operator_address(operator_variant);
}

void set_nested_config_path(ConfigAdapter *owner,
                            const std::string &owner_prefix,
                            const std::string &member) {
  if (!owner) return;
  const auto property_name = member + "Adapter";
  auto *nested = qobject_cast<ConfigAdapter *>(
      owner->property(property_name.c_str()).value<QObject *>());
  if (nested)
    nested->setConfigPath(QString::fromStdString(owner_prefix + "/" + member));
}

// for when whatever owns a set of paths moves to a new address
void reroot_paths_between(pc::WorkspaceConfiguration &config,
                          const std::string &previous_prefix,
                          const std::string &new_prefix) {
  if (new_prefix == previous_prefix) return;
  config.publish_paths.set(
      reroot_paths(config.publish_paths.value(), previous_prefix, new_prefix));
  config.push_paths.set(
      reroot_paths(config.push_paths.value(), previous_prefix, new_prefix));
  config.render_paths.set(
      reroot_paths(config.render_paths.value(), previous_prefix, new_prefix));
}

// for when a device or other node in the device list is renamed...
void reroot_node_paths(pc::WorkspaceConfiguration &config,
                       const std::string &previous_prefix,
                       const std::string &node_id) {
  reroot_paths_between(config, previous_prefix,
                       node_path_prefix(config, node_id));
}

void erase_session_node_state(pc::WorkspaceConfiguration &config,
                              const std::string &node_id) {
  for (auto &session_config : config.sessions) {
    auto disabled_nodes = session_config.disabled_devices.value();
    if (disabled_nodes.erase(node_id) > 0)
      session_config.disabled_devices.set(std::move(disabled_nodes));
  }
}

// for when a node is deleted
void erase_node_paths(pc::WorkspaceConfiguration &config,
                      const std::string &prefix) {
  const auto deleted = [&prefix](const std::string &path) {
    return path_under_prefix(path, prefix);
  };
  std::erase_if(config.publish_paths.value(), deleted);
  std::erase_if(config.push_paths.value(), deleted);
  std::erase_if(config.render_paths.value(), deleted);
}

// -------- undo commands --------

class SetSessionConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;

  SetSessionConfigCommand(QString session_id,
                          SessionConfiguration before_config,
                          SessionConfiguration after_config,
                          QString command_text,
                          pc::WorkspaceConfiguration base_config_snapshot,
                          ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)),
        _session_id(std::move(session_id)), _before(std::move(before_config)),
        _after(std::move(after_config)),
        _base_snapshot(std::move(base_config_snapshot)),
        _apply_fn(std::move(apply_fn)) {}

  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  QString _session_id;
  SessionConfiguration _before;
  SessionConfiguration _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;

  void apply(const SessionConfiguration &config_value) {
    auto new_config = _base_snapshot;

    const int idx =
        find_session_index_by_id(new_config, _session_id.toStdString());
    if (idx < 0 || idx >= int(new_config.sessions.size())) {
      pc::logger()->warn("SetSessionConfigCommand: session id not found '{}'",
                         _session_id.toStdString());
      return;
    }

    // a label edit moves the session's address, so its publish/push paths go
    // with it, in both directions since undo restores the old label
    const auto previous_prefix =
        session_path_prefix(new_config, _session_id.toStdString());

    // the same goes for an operator's own label
    std::unordered_map<std::string, std::string> previous_operator_nodes;
    for (const auto &operator_variant :
         new_config.sessions[size_t(idx)].operators) {
      previous_operator_nodes.emplace(
          std::get<0>(operators::operator_info_from_variant(operator_variant)),
          operators::operator_address(operator_variant));
    }

    new_config.sessions[size_t(idx)] = config_value;

    const auto new_prefix =
        session_path_prefix(new_config, _session_id.toStdString());
    reroot_paths_between(new_config, previous_prefix, new_prefix);

    for (const auto &operator_variant :
         new_config.sessions[size_t(idx)].operators) {
      const auto previous_node = previous_operator_nodes.find(
          std::get<0>(operators::operator_info_from_variant(operator_variant)));
      if (previous_node == previous_operator_nodes.end()) continue;
      reroot_paths_between(new_config,
                           new_prefix + "/operators/" + previous_node->second,
                           operator_path_prefix(new_prefix, operator_variant));
    }

    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetDeviceConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;

  SetDeviceConfigCommand(QString device_id,
                         pc::devices::DeviceConfigurationVariant before_config,
                         pc::devices::DeviceConfigurationVariant after_config,
                         QString command_text,
                         pc::WorkspaceConfiguration base_config_snapshot,
                         ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _device_id(std::move(device_id)),
        _before(std::move(before_config)), _after(std::move(after_config)),
        _base_snapshot(std::move(base_config_snapshot)),
        _apply_fn(std::move(apply_fn)) {}

  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  QString _device_id;
  pc::devices::DeviceConfigurationVariant _before;
  pc::devices::DeviceConfigurationVariant _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;

  void apply(const pc::devices::DeviceConfigurationVariant &config_value) {
    auto new_config = _base_snapshot;
    const int idx =
        find_device_index_by_id(new_config, _device_id.toStdString());
    if (idx < 0 || idx >= int(new_config.devices.size())) {
      pc::logger()->warn("SetDeviceConfigCommand: device id not found '{}'",
                         _device_id.toStdString());
      return;
    }
    new_config.devices[size_t(idx)] = config_value;
    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetDeviceLabelCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;

  // the publish/push paths are rooted at the device's address, so they have to
  // travel with the label through undo and redo
  struct State {
    std::string label;
    std::set<std::string> publish_paths;
    std::set<std::string> push_paths;
  };

  SetDeviceLabelCommand(QString device_id, State before_state,
                        State after_state, QString command_text,
                        pc::WorkspaceConfiguration base_config_snapshot,
                        ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _device_id(std::move(device_id)),
        _before(std::move(before_state)), _after(std::move(after_state)),
        _base_snapshot(std::move(base_config_snapshot)),
        _apply_fn(std::move(apply_fn)) {}

  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  QString _device_id;
  State _before;
  State _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;

  void apply(const State &state) {
    auto new_config = _base_snapshot;
    const int idx =
        find_device_index_by_id(new_config, _device_id.toStdString());
    if (idx < 0 || idx >= int(new_config.devices.size())) {
      pc::logger()->warn("SetDeviceLabelCommand: device id not found '{}'",
                         _device_id.toStdString());
      return;
    }
    std::visit(
        [&](auto &device_config) { device_config.label.set(state.label); },
        new_config.devices[size_t(idx)]);
    new_config.publish_paths.set(state.publish_paths);
    new_config.push_paths.set(state.push_paths);
    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetDeviceGroupConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;

  SetDeviceGroupConfigCommand(QString group_id,
                              pc::devices::DeviceGroupConfiguration before,
                              pc::devices::DeviceGroupConfiguration after,
                              QString command_text,
                              pc::WorkspaceConfiguration base_snapshot,
                              ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _group_id(std::move(group_id)),
        _before(std::move(before)), _after(std::move(after)),
        _base_snapshot(std::move(base_snapshot)),
        _apply_fn(std::move(apply_fn)) {}

  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  QString _group_id;
  pc::devices::DeviceGroupConfiguration _before;
  pc::devices::DeviceGroupConfiguration _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;

  void apply(const pc::devices::DeviceGroupConfiguration &value) {
    auto new_config = _base_snapshot;
    const int gi =
        find_device_group_index_by_id(new_config, _group_id.toStdString());
    if (gi < 0 || gi >= int(new_config.device_groups.size())) return;
    new_config.device_groups[size_t(gi)] = value;
    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetOperatorConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;
  SetOperatorConfigCommand(
      QString device_id, QString operator_id,
      pc::operators::OperatorConfigurationVariant before_config,
      pc::operators::OperatorConfigurationVariant after_config,
      QString command_text, pc::WorkspaceConfiguration base_config_snapshot,
      ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _device_id(std::move(device_id)),
        _operator_id(std::move(operator_id)), _before(std::move(before_config)),
        _after(std::move(after_config)),
        _base_snapshot(std::move(base_config_snapshot)),
        _apply_fn(std::move(apply_fn)) {}
  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  QString _device_id;
  QString _operator_id;
  pc::operators::OperatorConfigurationVariant _before;
  pc::operators::OperatorConfigurationVariant _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;
  void apply(const pc::operators::OperatorConfigurationVariant &config_value) {
    auto new_config = _base_snapshot;
    const int device_index =
        find_device_index_by_id(new_config, _device_id.toStdString());
    if (device_index < 0 || device_index >= int(new_config.devices.size()))
      return;
    const int op_idx = find_operator_index_by_id(
        new_config.devices[size_t(device_index)], _operator_id.toStdString());
    if (op_idx < 0) return;

    // a label edit moves the operator's address under its device, so its
    // publish/push paths travel with it
    std::string previous_node;
    std::visit(
        [&](auto &device_config) {
          if constexpr (requires { device_config.operators; }) {
            auto &operator_variant = device_config.operators[size_t(op_idx)];
            previous_node = operators::operator_address(operator_variant);
            operator_variant = config_value;
          }
        },
        new_config.devices[size_t(device_index)]);

    if (!previous_node.empty()) {
      const auto owner_prefix =
          node_path_prefix(new_config, _device_id.toStdString());
      reroot_paths_between(new_config,
                           owner_prefix + "/operators/" + previous_node,
                           operator_path_prefix(owner_prefix, config_value));
    }

    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetPointStreamerConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;
  SetPointStreamerConfigCommand(
      pc::networking::PointStreamerConfiguration before,
      pc::networking::PointStreamerConfiguration after, QString command_text,
      pc::WorkspaceConfiguration base_snapshot, ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _before(std::move(before)),
        _after(std::move(after)), _base_snapshot(std::move(base_snapshot)),
        _apply_fn(std::move(apply_fn)) {}
  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  pc::networking::PointStreamerConfiguration _before;
  pc::networking::PointStreamerConfiguration _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;
  void apply(const pc::networking::PointStreamerConfiguration &value) {
    auto new_config = _base_snapshot;
    new_config.point_streamer.set(value);
    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

class SetPublishersConfigCommand final : public QUndoCommand {
public:
  using ApplyFn = std::function<void(pc::WorkspaceConfiguration)>;
  SetPublishersConfigCommand(pc::publishers::PublishersConfiguration before,
                             pc::publishers::PublishersConfiguration after,
                             QString command_text,
                             pc::WorkspaceConfiguration base_snapshot,
                             ApplyFn apply_fn)
      : QUndoCommand(std::move(command_text)), _before(std::move(before)),
        _after(std::move(after)), _base_snapshot(std::move(base_snapshot)),
        _apply_fn(std::move(apply_fn)) {}
  void undo() override { apply(_before); }
  void redo() override { apply(_after); }

private:
  pc::publishers::PublishersConfiguration _before;
  pc::publishers::PublishersConfiguration _after;
  pc::WorkspaceConfiguration _base_snapshot;
  ApplyFn _apply_fn;
  void apply(const pc::publishers::PublishersConfiguration &value) {
    auto new_config = _base_snapshot;
    new_config.publishers.set(value);
    if (_apply_fn) _apply_fn(std::move(new_config));
  }
};

struct SessionUpdateGate {
  std::atomic<bool> pending{false};
  QPointer<SessionAdapter> adapter;
};

} // namespace

// ----------------- WorkspaceModel helpers -----------------

QString WorkspaceModel::adapterStableId(ConfigAdapter *adapter) {
  if (!adapter) return {};
  const QVariant v = adapter->value(QStringLiteral("id"));
  if (v.isValid()) return v.toString();
  return {};
}

QString WorkspaceModel::adapterStableId(DeviceAdapter *adapter) {
  if (!adapter) return {};
  const QString cached_id = adapter->deviceId();
  if (!cached_id.isEmpty()) return cached_id;
  const QVariant v = adapter->value(QStringLiteral("id"));
  if (v.isValid()) return v.toString();
  return {};
}

void WorkspaceModel::applyWorkspaceConfigAndRebuild(
    pc::WorkspaceConfiguration new_config, RebuildScope scope) {
  const bool sync_devices =
      (scope == RebuildScope::Devices || scope == RebuildScope::All);
  {
    std::scoped_lock lock(_workspace.config_access);
    if (_pointStreamerAdapter) {
      _pointStreamerAdapter->setConfig(
          pc::ConfigurationVariant{new_config.point_streamer.value()});
    }
    if (_publishersConfigAdapter) {
      _publishersConfigAdapter->setConfig(
          pc::ConfigurationVariant{new_config.publishers.value()});
    }
  }
  _workspace.apply_new_config(std::move(new_config), sync_devices);

  if (sync_devices) {
    // first look at the live plugins and invalidate adapters for any that
    // aren't loaded / have been deleted
    std::unordered_set<const void *> live_plugins;
    live_plugins.reserve(_workspace.devices.size());
    for (auto &device_plugin : _workspace.devices) {
      if (device_plugin) live_plugins.insert(device_plugin.get());
    }
    for (QObject *obj : _deviceAdapters) {
      auto *a = qobject_cast<DeviceAdapter *>(obj);
      if (!a || !a->plugin()) continue;
      if (!live_plugins.contains(a->plugin())) a->invalidatePlugin();
    }
  }

  // then invalidate adapters for any sessions that have been deleted, since a
  // queued point cloud update can still reach one before it is deleted
  for (const auto &[id, adapter] :
       std::as_const(_sessionPointCloudAdapters).asKeyValueRange()) {
    if (adapter && !_workspace.sessions.contains(id.toStdString())) {
      adapter->invalidateSession();
    }
  }

  switch (scope) {
  case RebuildScope::None: {
    break;
  }
  case RebuildScope::Sessions: {
    syncSessionAdapters();
    break;
  }
  case RebuildScope::Devices: {
    syncDeviceAdapters();
    break;
  }
  case RebuildScope::All: {
    syncAdapters();
    break;
  }
  }
  emit publishPathsChanged();
  emit pushPathsChanged();
  emit renderPathsChanged();
  _streamChannelModel->refresh();
}
// ----------------- WorkspaceModel -----------------
WorkspaceModel::WorkspaceModel(pc::Workspace *workspace, QObject *parent)

    : QObject(parent), _workspace(*workspace) {
  if (workspace->auto_loaded_config) {
    const auto path = AppSettings::instance()->lastWorkspacePath();
    setSaveFileUrl(QUrl::fromLocalFile(path));
  }

  _recorderModel = new RecorderModel(workspace->session_recorder.get(), this);
  emit recorderChanged();

  _pointStreamerAdapter = new pc::networking::PointStreamerConfigurationAdapter(
      _workspace.config.point_streamer.value(), this);
  _pointStreamerAdapter->setConfigPath(QStringLiteral("streaming"));
  initPointStreamerAdapter();

  _publishersConfigAdapter = new pc::publishers::PublishersConfigurationAdapter(
      _workspace.config.publishers.value(), this);
  _publishersConfigAdapter->setConfigPath(QStringLiteral("publishers"));
  initPublishersConfigAdapter();

  _streamChannelModel = new StreamChannelListModel(workspace, this);
  _streamChannelModel->refresh();

  _logModel = new LogModel(this);

  connect(_undoStack, &QUndoStack::cleanChanged, this,
          [this](bool) { emit dirtyChanged(); });
}

WorkspaceModel::~WorkspaceModel() {
  // every prefix starts with "", so this drops all of our subscriptions
  unsubscribeFromRegistry("");
}

void WorkspaceModel::close() {
  QCoreApplication::quit();
}

void WorkspaceModel::registerPluginSettingsPages() {
  for (const auto &[plugin_name, plugin] : _workspace.discovery_plugins) {
    if (!plugin) continue;
    for (const auto &page : plugin->settings_pages()) {
      pc::logger()->trace("Registering settings page '{}' from plugin '{}'",
                          page.key, plugin_name);
      SettingsPageRegistry::instance()->addSection("Devices");
      SettingsPageRegistry::instance()->addPage(
          QString::fromStdString(page.key), QString::fromStdString(page.title),
          QUrl::fromLocalFile(QString::fromStdString(page.qml_file_path)));
    }
  }
}

void WorkspaceModel::loadFromFile(const QUrl &file) {
  const QString local_path = file.toLocalFile();
  if (local_path.isEmpty()) return;

  const bool load_malformed =
      AppSettings::instance()->loadMalformedWorkspaces();
  std::jthread([this, file, local_path, load_malformed]() mutable {
    pc::WorkspaceConfiguration loaded_config;
    pc::load_workspace_from_file(loaded_config, local_path.toStdString(),
                                 load_malformed);
    QMetaObject::invokeMethod(
        this,
        [this, file, local_path, config = std::move(loaded_config)]() mutable {
          setSaveFileUrl(file);
          applyWorkspaceConfigAndRebuild(std::move(config));
          _undoStack->clear();
          AppSettings::instance()->setlastWorkspacePath(local_path);
        },
        Qt::QueuedConnection);

    std::filesystem::path session_file_path{local_path.toStdString()};
    auto layout_file_path = session_file_path;
    layout_file_path.replace_filename(session_file_path.stem().string() +
                                      "_layout.json");

    if (std::filesystem::exists(layout_file_path)) {
      QMetaObject::invokeMethod(
          this,
          [fp = layout_file_path.string(), this] {
            pc::ui::LayoutSaver saver(this);
            saver.load_file(fp.c_str());
          },
          Qt::QueuedConnection);
    }
  }).detach();
}

void WorkspaceModel::newWorkspace() {
  QMetaObject::invokeMethod(
      this,
      [this]() {
        setSaveFileUrl({});
        applyWorkspaceConfigAndRebuild(
            {.sessions = {
                 {.id = pc::uuid::word(),
                  .label = "session_1",
                  .camera = CameraConfiguration{.id = pc::uuid::word()}}}});
        setSelectedDeviceIndex(-1);
        _undoStack->clear();
        emit newWorkspaceLoaded();
      },
      Qt::QueuedConnection);
}

void WorkspaceModel::save(bool update_last_session_path) {
  if (saveFileUrl().isEmpty()) {
    emit openSaveAsDialog();
    return;
  }

  QString local_path = saveFileUrl().toLocalFile();
  if (local_path.isEmpty()) {
    emit openSaveAsDialog();
    return;
  }

  using namespace Qt::StringLiterals;
  if (!local_path.endsWith(u".yaml"_s, Qt::CaseInsensitive)) {
    local_path += u".yaml"_s;
  }

  std::jthread([workspace_config = _workspace.config, local_path,
                update_last_session_path, this] {
    save_workspace_to_file(workspace_config, local_path.toStdString());
    QMetaObject::invokeMethod(
        this, [this] { _undoStack->setClean(); }, Qt::QueuedConnection);
    if (update_last_session_path) {
      AppSettings::instance()->setlastWorkspacePath(local_path);
    }

    constexpr bool save_adjacent_layout_file = true;
    if (save_adjacent_layout_file) {
      std::filesystem::path session_file_path{local_path.toStdString()};
      auto layout_file_path = session_file_path;
      layout_file_path.replace_filename(session_file_path.stem().string() +
                                        "_layout.json");
      QMetaObject::invokeMethod(
          this,
          [this, save_path = layout_file_path.string()]() {
            pc::ui::LayoutSaver saver(this);
            const bool result = saver.save_file(save_path.c_str());
          },
          Qt::QueuedConnection);
    }
  }).detach();
}

QVariantList WorkspaceModel::availableOperators() const {
  static const QRegularExpression camel_boundary(
      QStringLiteral("([a-z0-9])([A-Z])"));
  QVariantList entries;
  for (const auto &plugin_name : _workspace.loaded_operator_plugin_names) {
    const QString name = QString::fromStdString(plugin_name);
    QString label = name;
    if (label.endsWith(QStringLiteral("Operator"))) {
      label.chop(QStringLiteral("Operator").size());
    }
    label.replace(camel_boundary, QStringLiteral("\\1 \\2"));
    entries.append(QVariantMap{{QStringLiteral("name"), name},
                               {QStringLiteral("label"), label}});
  }
  return entries;
}

void WorkspaceModel::setImageProvider(CameraImageProvider *provider) {
  if (_imageProvider == provider) return;
  _imageProvider = provider;
  if (_imageProvider) {
    syncAdapters();
  }
}

namespace {
QStringList to_string_list(const std::set<std::string> &paths) {
  QStringList list;
  list.reserve(int(paths.size()));
  for (const auto &path : paths) list.append(QString::fromStdString(path));
  return list;
}
} // namespace

QStringList WorkspaceModel::publishPaths() const {
  std::scoped_lock lock(_workspace.config_access);
  return to_string_list(_workspace.config.publish_paths.value());
}

QStringList WorkspaceModel::pushPaths() const {
  std::scoped_lock lock(_workspace.config_access);
  return to_string_list(_workspace.config.push_paths.value());
}

QStringList WorkspaceModel::renderPaths() const {
  std::scoped_lock lock(_workspace.config_access);
  return to_string_list(_workspace.config.render_paths.value());
}

// a field's entry is its adapter's configPath joined with the field path.
// RebuildScope::None because no adapter or device is affected by the sets
// changing, only the rows drawing their published state
void WorkspaceModel::addPublishPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.publish_paths.value().insert(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

void WorkspaceModel::removePublishPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.publish_paths.value().erase(path.toStdString());
  // a path cannot be pushed if it is not also published
  new_config.push_paths.value().erase(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

void WorkspaceModel::addPushPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.push_paths.value().insert(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

void WorkspaceModel::removePushPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.push_paths.value().erase(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

void WorkspaceModel::addRenderPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.render_paths.value().insert(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

void WorkspaceModel::removeRenderPath(const QString &path) {
  auto new_config = _workspace.config;
  new_config.render_paths.value().erase(path.toStdString());
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
}

StreamSource *WorkspaceModel::streamSourceFor(const QString &path) {
  auto it = _streamSources.find(path);
  if (it != _streamSources.end()) return it.value();
  auto *source =
      new StreamSource(_workspace.config_registry, path.toStdString(), this);
  _streamSources.insert(path, source);
  return source;
}

// the device or group a rendered path belongs to, or empty for a path that
// belongs to no node in the tree
QString WorkspaceModel::renderPathOwnerId(const QString &path) const {
  const std::string target_path = path.toStdString();
  if (!target_path.starts_with("device/") ||
      target_path.contains("/operators/"))
    return {};

  std::scoped_lock lock(_workspace.config_access);
  return QString::fromStdString(
      render_path_node_id(_workspace.config, target_path));
}

// the world transform of the space a rendered path's value sits in. a device
// or group crops its points in its parent's space, so a box of theirs is drawn
// under the ancestor transform, while an operator sees a cloud that has
// already been world-transformed and an output stream comes out the same way
QMatrix4x4 WorkspaceModel::renderPathParentWorld(const QString &path) const {
  const QString node_id = renderPathOwnerId(path);
  if (node_id.isEmpty()) return {};
  // nodeAncestorWorldMatrix takes the config lock itself
  return nodeAncestorWorldMatrix(node_id);
}

// and the transform of the node itself, for the values that sit in its own
// space rather than its parent's. a sensor's reach is measured from where the
// sensor is, so a disc of one turns and travels with the device
QMatrix4x4 WorkspaceModel::renderPathOwnerWorld(const QString &path) const {
  return nodeWorldMatrix(renderPathOwnerId(path));
}

QMatrix4x4 WorkspaceModel::nodeWorldMatrix(const QString &node_id) const {
  if (node_id.isEmpty()) return {};

  const QMatrix4x4 ancestors = nodeAncestorWorldMatrix(node_id);

  pc::TransformConfiguration local;
  {
    std::scoped_lock lock(_workspace.config_access);
    const std::string id = node_id.toStdString();
    if (const int group_index =
            pc::devices::group_index_by_id(_workspace.config, id);
        group_index >= 0) {
      local = _workspace.config.device_groups[std::size_t(group_index)]
                  .transform.value();
    } else {
      const int device_index = find_device_index_by_id(_workspace.config, id);
      if (device_index < 0) return ancestors;
      std::visit(
          [&](const auto &device_config) {
            local = device_config.transform.value();
          },
          _workspace.config.devices[std::size_t(device_index)]);
    }
  }

  std::array<float, 16> v = pc::to_float4x4(local).values;
  // a local transform carries its translation in millimetres
  v[3] *= 0.001f;
  v[7] *= 0.001f;
  v[11] *= 0.001f;
  return ancestors * QMatrix4x4(v.data());
}

// a scene object draws whatever sits at its path, and unlike a stream, a value
// a configuration carries only moves when something edits it. node addresses
// nest, so an edit under one node reaches its descendants' paths too, and a
// group moving takes the boxes of the devices beneath it along
void WorkspaceModel::watchStreamSourcesFor(ConfigAdapter *adapter) {
  if (!adapter) return;
  // adapters are re-pathed on every sync, so drop any previous hookup first
  QObject::disconnect(adapter, &ConfigAdapter::fieldChanged, this, nullptr);
  // queued because an edit reaches an operator's registered configuration on
  // the way back out of the handler that announced the field, so reading it
  // any earlier hands the scene the value it is replacing
  QObject::connect(
      adapter, &ConfigAdapter::fieldChanged, this,
      [this, adapterPtr = QPointer<ConfigAdapter>(adapter)]() {
        if (!adapterPtr) return;
        const QString prefix = adapterPtr->configPath() + "/";
        for (auto *source : _streamSources) {
          if (source && source->path().startsWith(prefix))
            source->notifyChanged();
        }
      },
      Qt::QueuedConnection);
}

QList<QObject *> WorkspaceModel::sessionAdapters() const {
  return _sessionAdapters;
}

void WorkspaceModel::setSelectedSessionId(const QString &id) {
  if (_selectedSessionId == id) return;
  _selectedSessionId = id;
  _workspace.config.selectedSessionId.set(id.toStdString());

  if (_selectedOperatorAdapter) {
    for (auto session_operators = _sessionOperatorAdapters.begin();
         session_operators != _sessionOperatorAdapters.end();
         ++session_operators) {
      if (session_operators.key() == id) continue;
      if (session_operators.value().contains(_selectedOperatorAdapter)) {
        setSelectedOperatorAdapter(nullptr);
        break;
      }
    }
  }

  emit selectedSessionChanged();
  emit deviceTreeRowsChanged();
}

QString WorkspaceModel::sessionIdForLabel(const QString &label) const {
  const auto target_label = label.toStdString();
  std::scoped_lock lock(_workspace.config_access);
  for (const auto &session_config : _workspace.config.sessions) {
    if (pc::session_address(session_config) == target_label)
      return QString::fromStdString(session_config.id);
  }
  return {};
}

QString WorkspaceModel::addSession() {
  auto new_config = _workspace.config;

  pc::SessionConfiguration new_session{
      .id = pc::uuid::word(),
      .camera = CameraConfiguration{.id = pc::uuid::word()}};
  new_session.label.set(util::next_available_label(
      "session",
      new_config.sessions |
          std::views::transform([](const pc::SessionConfiguration &session) {
            return session.label.value();
          })));

  // a new session starts with nothing switched on
  const auto node_ids = pc::devices::all_node_ids(new_config);
  new_session.disabled_devices.set(
      std::set<std::string>(node_ids.begin(), node_ids.end()));

  const QString new_session_id = QString::fromStdString(new_session.id);
  new_config.sessions.push_back(std::move(new_session));
  new_config.selectedSessionId.set(new_session_id.toStdString());

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Sessions);
  setSelectedSessionId(new_session_id);
  emit sessionAdded(new_session_id);
  return new_session_id;
}

void WorkspaceModel::removeSession(const QString &sessionId) {
  QMetaObject::invokeMethod(
      this,
      [this, removed_session_id = sessionId.toStdString()]() {
        auto new_config = _workspace.config;
        // a workspace always keeps one session
        if (new_config.sessions.size() <= 1) return;

        const int found_index =
            find_session_index_by_id(new_config, removed_session_id);
        if (found_index < 0) return;
        const auto session_index = static_cast<size_t>(found_index);

        const auto session_prefix =
            session_path_prefix(new_config, removed_session_id);
        new_config.sessions.erase(new_config.sessions.begin() + session_index);
        erase_node_paths(new_config, session_prefix);

        QString next_selected_session_id = _selectedSessionId;
        if (next_selected_session_id == removed_session_id ||
            next_selected_session_id.isEmpty()) {
          const auto next_index =
              std::min(session_index, new_config.sessions.size() - 1);
          next_selected_session_id =
              QString::fromStdString(new_config.sessions[next_index].id);
        }
        new_config.selectedSessionId.set(
            next_selected_session_id.toStdString());

        applyWorkspaceConfigAndRebuild(std::move(new_config),
                                       RebuildScope::Sessions);
        setSelectedSessionId(next_selected_session_id);
      },
      Qt::QueuedConnection);
}

void WorkspaceModel::duplicateSession(const QString &sessionId) {
  QMetaObject::invokeMethod(
      this,
      [this, session_id = sessionId.toStdString()]() {
        auto new_config = _workspace.config;
        const int session_index =
            find_session_index_by_id(new_config, session_id);
        if (session_index < 0) return;

        auto session_copy =
            new_config.sessions[static_cast<size_t>(session_index)];
        session_copy.id = pc::uuid::word();
        session_copy.camera.value().id = pc::uuid::word();
        for (auto &operator_variant : session_copy.operators) {
          std::visit(
              [](auto &operator_config) {
                operator_config.id = pc::uuid::word();
              },
              operator_variant);
        }
        session_copy.label.set(util::next_available_label(
            "session", new_config.sessions |
                           std::views::transform(
                               [](const pc::SessionConfiguration &session) {
                                 return session.label.value();
                               })));

        const QString new_session_id = QString::fromStdString(session_copy.id);
        new_config.sessions.push_back(std::move(session_copy));
        new_config.selectedSessionId.set(new_session_id.toStdString());

        applyWorkspaceConfigAndRebuild(std::move(new_config),
                                       RebuildScope::Sessions);
        setSelectedSessionId(new_session_id);
        emit sessionAdded(new_session_id);
      },
      Qt::QueuedConnection);
}

// renaming retitles the tab whose editor is calling this
void WorkspaceModel::setSessionLabel(const QString &sessionId,
                                     const QString &label) {
  QMetaObject::invokeMethod(
      this,
      [this, renamed_session_id = sessionId.toStdString(),
       new_session_label = label.toStdString()]() {
        auto new_config = _workspace.config;
        const int found_index =
            find_session_index_by_id(new_config, renamed_session_id);
        if (found_index < 0) return;
        const auto session_index = static_cast<size_t>(found_index);

        // make sure of no duplicates
        if (new_session_label.empty()) return;
        for (size_t i = 0; i < new_config.sessions.size(); i++) {
          if (i == session_index) continue;
          if (new_config.sessions[i].label.value() == new_session_label) return;
        }

        const auto previous_prefix =
            session_path_prefix(new_config, renamed_session_id);
        new_config.sessions[session_index].label.set(
            std::move(new_session_label));
        reroot_paths_between(
            new_config, previous_prefix,
            session_path_prefix(new_config, renamed_session_id));
        applyWorkspaceConfigAndRebuild(std::move(new_config),
                                       RebuildScope::Sessions);
      },
      Qt::QueuedConnection);
}

bool WorkspaceModel::sessionDeviceEnabled(const QString &node_id) const {
  std::scoped_lock lock(_workspace.config_access);
  const int session_index = find_session_index_by_id(
      _workspace.config, _selectedSessionId.toStdString());
  if (session_index < 0) return true;
  const auto &session_config =
      _workspace.config.sessions[static_cast<size_t>(session_index)];
  return !session_config.disabled_devices.value().contains(
      node_id.toStdString());
}

bool WorkspaceModel::sessionDrawsRenderPath(const QString &sessionId,
                                            const QString &path) const {
  const std::string target_path = path.toStdString();

  std::scoped_lock lock(_workspace.config_access);
  const int session_index =
      find_session_index_by_id(_workspace.config, sessionId.toStdString());
  if (session_index < 0) return true;
  const auto &session_config =
      _workspace.config.sessions[static_cast<size_t>(session_index)];

  if (target_path.starts_with("session/")) {
    return target_path.starts_with(
        session_path_prefix(_workspace.config, session_config.id) + "/");
  }
  if (!target_path.starts_with("device/")) return true;

  const auto owner_node_id =
      render_path_node_id(_workspace.config, target_path);
  if (owner_node_id.empty()) return true;
  return pc::devices::effective_session_enabled(_workspace.config,
                                                session_config, owner_node_id);
}

void WorkspaceModel::setSessionDeviceEnabled(const QString &node_id,
                                             bool enabled) {
  auto new_config = _workspace.config;
  const int session_index =
      find_session_index_by_id(new_config, _selectedSessionId.toStdString());
  if (session_index < 0) return;

  auto &session_config =
      new_config.sessions[static_cast<size_t>(session_index)];
  auto disabled_nodes = session_config.disabled_devices.value();
  const auto target_node_id = node_id.toStdString();
  if (enabled) {
    if (disabled_nodes.erase(target_node_id) == 0) return;
  } else {
    if (!disabled_nodes.insert(target_node_id).second) return;
  }
  session_config.disabled_devices.set(std::move(disabled_nodes));

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Sessions);
  emit deviceTreeRowsChanged();
}

QObject *WorkspaceModel::selectedSessionAdapter() const {
  if (auto *adapter = _sessionAdapterById.value(_selectedSessionId).data())
    return adapter;
  // fall back to the first session when no (valid) selection is set
  return _sessionAdapters.isEmpty() ? nullptr : _sessionAdapters.first();
}

QList<OperatorAdapter *>
WorkspaceModel::sessionOperatorAdaptersFor(const QString &sessionId) const {
  return _sessionOperatorAdapters.value(sessionId);
}

QList<OperatorAdapter *>
WorkspaceModel::selectedSessionOperatorAdapters() const {
  QString id = _selectedSessionId;
  if (id.isEmpty() || !_sessionOperatorAdapters.contains(id)) {
    if (!_sessionAdapters.isEmpty()) {
      auto *a =
          qobject_cast<SessionConfigurationAdapter *>(_sessionAdapters.first());
      if (a) id = a->id();
    }
  }
  return _sessionOperatorAdapters.value(id);
}

QVariant WorkspaceModel::deviceAdapters() const {
  return QVariant::fromValue(_deviceAdapters);
}

QStringList WorkspaceModel::deviceVariantNames() const {
  QStringList names;
  names.reserve(static_cast<int>(
      std::variant_size_v<pc::devices::DeviceConfigurationVariant>));
  pc::devices::for_each_device_config_type(
      [&names]<typename DeviceConfigType>() {
        if constexpr (requires { DeviceConfigType::PluginName; }) {
          names.push_back(QString::fromStdString(DeviceConfigType::PluginName));
        }
      });
  return names;
}

QVariantList WorkspaceModel::addDeviceMenuEntries() const {
  QVariantList menu_entries;

  auto &existing_devices = _workspace.devices;

  const auto existing_device_ids =
      std::ranges::transform_view(existing_devices, [](auto &device) {
        return std::visit(
            [&](auto &device_config) -> std::string_view {
              return device_config.id;
            },
            device->config_variant());
      });

  for (auto id : existing_device_ids) {
    pc::logger()->debug("Existing id: {}", id);
  }

  for (auto plugin_name : _workspace.loaded_device_plugin_names) {

    if (plugin_name == "NullDevice") continue;

    QVariantMap entry;
    entry["plugin_name"] = QString::fromStdString(plugin_name);
    entry["kind"] = "plugin";
    menu_entries.push_back(std::move(entry));

    // any discovered network devices of this plugin type
    auto it = _workspace.discovery_plugins.find(plugin_name);
    if (it == _workspace.discovery_plugins.end() || !it->second) continue;
    const auto discovered_devices = it->second->discovered_devices();
    for (const auto &device : discovered_devices) {

      // skip devices we're already connected to
      if (std::ranges::any_of(existing_device_ids,
                              [&](auto id) { return id == device.id; })) {
        continue;
      }

      QVariantMap discovered_device_entry;

      discovered_device_entry["kind"] = "discovered";
      discovered_device_entry["plugin_name"] =
          QString::fromStdString(plugin_name);
      discovered_device_entry["ip"] = QString::fromStdString(device.ip);
      discovered_device_entry["id"] = QString::fromStdString(device.id);
      discovered_device_entry["label"] = QString::fromStdString(device.label);
      discovered_device_entry["type_label"] =
          QString::fromStdString(device.type_label);

      menu_entries.push_back(std::move(discovered_device_entry));
    }
  }

  return menu_entries;
}

void WorkspaceModel::setSelectedStreamChannelIndex(int index) {
  const int clamped =
      std::clamp(index, -1, _streamChannelModel->rowCount() - 1);
  if (_selectedStreamChannelIndex == clamped) return;
  _selectedStreamChannelIndex = clamped;
  emit selectedStreamChannelIndexChanged();
}

void WorkspaceModel::setSelectedDeviceIndex(int index) {
  const QString node_id = adapterStableId(deviceAdapterAt(index));
  const QString node_kind =
      node_id.isEmpty() ? QString() : QStringLiteral("device");

  if (_selectedDeviceIndex == index && _selectedNodeId == node_id &&
      _selectedNodeKind == node_kind)
    return;

  _selectedDeviceIndex = index;
  _workspace.config.selectedDeviceIndex = index;
  setSelectedOperatorAdapter(nullptr);

  _selectedNodeId = node_id;
  _selectedNodeKind = node_kind;

  emit selectedDeviceIndexChanged();
  emit selectedNodeChanged();

  rebuildSelectedGroupAdapter();
}

void WorkspaceModel::selectNode(const QString &node_id) {
  const std::string id = node_id.toStdString();
  if (id.empty()) return;

  // group node: leave the device index alone, switch the editor by kind
  if (find_device_group_index_by_id(_workspace.config, id) >= 0) {
    if (_selectedNodeKind == QStringLiteral("group") &&
        _selectedNodeId == node_id)
      return;
    setSelectedOperatorAdapter(nullptr);
    _selectedNodeId = node_id;
    _selectedNodeKind = QStringLiteral("group");
    rebuildSelectedGroupAdapter();
    emit selectedNodeChanged();
    return;
  }

  // device node: route through the existing device-selection path
  const int device_index = find_device_index_by_id(_workspace.config, id);
  if (device_index >= 0) setSelectedDeviceIndex(device_index);
}

QMatrix4x4
WorkspaceModel::nodeAncestorWorldMatrix(const QString &node_id) const {
  pc::float4x4 world;
  {
    std::scoped_lock lock(_workspace.config_access);
    world = pc::devices::effective_world_transform(_workspace.config,
                                                   node_id.toStdString());
  }
  std::array<float, 16> v = world.values;
  v[3] *= 0.001f;
  v[7] *= 0.001f;
  v[11] *= 0.001f;
  return QMatrix4x4(v.data());
}

QVariantMap WorkspaceModel::localTransformForWorldAlignment(
    const QString &device_id, const QMatrix4x4 &worldAlignment) const {
  QVariantMap result;

  const auto id = device_id.toStdString();

  pc::TransformConfiguration device_transform;
  pc::float4x4 world;
  {
    std::scoped_lock lock(_workspace.config_access);
    const int index = find_device_index_by_id(_workspace.config, id);
    if (index < 0) {
      pc::logger()->error("Alignment: no device '{}' to place", id);
      return result;
    }
    std::visit(
        [&](const auto &device_config) {
          device_transform = device_config.transform.value();
        },
        _workspace.config.devices[size_t(index)]);
    world = pc::devices::effective_world_transform(_workspace.config, id);
  }

  // the qt 3d scene uses centimeters, but point clouds
  // are all in milimeter shorts, so multiply cm -> mm
  QMatrix4x4 alignment = worldAlignment;
  alignment(0, 3) *= 10.0f;
  alignment(1, 3) *= 10.0f;
  alignment(2, 3) *= 10.0f;

  const QMatrix4x4 ancestors(world.values.data());

  bool invertible = false;
  const QMatrix4x4 ancestors_inverse = ancestors.inverted(&invertible);
  if (!invertible) {
    pc::logger()->error(
        "Alignment: '{}' sits under a group transform that can't be inverted",
        id);
    return result;
  }

  // the device's own placement without its scale
  auto rigid_transform = device_transform;
  rigid_transform.scale.set(pc::float3{1.0f, 1.0f, 1.0f});
  const QMatrix4x4 local(pc::to_float4x4(rigid_transform).values.data());

  // pull the world-space delta back into the device's parent space before
  // stacking it on what the device already has
  const QMatrix4x4 placed = ancestors_inverse * alignment * ancestors * local;

  pc::float4x4 placed_matrix;
  placed.copyDataTo(placed_matrix.values.data());

  const auto [position, rotation] = pc::decompose_transform(placed_matrix);
  result["position"] = QVector3D(position.x, position.y, position.z) * 0.001f;
  result["rotation"] =
      QQuaternion(rotation.scalar, rotation.x, rotation.y, rotation.z)
          .toEulerAngles();
  return result;
}

void WorkspaceModel::addNewDevice(const QString &plugin_name,
                                  const QString &target_ip,
                                  const QString &target_id,
                                  const QString &target_type_label) {
  // take a copy of current configuration to manipulate
  auto result_config = _workspace.config;
  devices::DeviceConfigurationVariant new_device_config;
  std::string_view plugin_prefix;
  // TODO this needs to be polymorphic runtime access
  if (plugin_name == OrbbecDeviceConfiguration::PluginName) {

    OrbbecDeviceConfiguration::SensorConfigurationVariant sensor_config;
    if (QString::compare(target_type_label, "rgbd") == 0) {
      sensor_config = OrbbecDeviceConfiguration::RgbdSensorConfiguration{};
    } else if (QString::compare(target_type_label, "lidar") == 0) {
      sensor_config = OrbbecDeviceConfiguration::LidarSensorConfiguration{};
    } else {
      pc::logger()->error("Invalid Orbbec device type.");
      return;
    }

    OrbbecDeviceConfiguration orbbec_config{
        .id = target_id.isEmpty() ? pc::uuid::word() : target_id.toStdString(),
        .sensor = std::move(sensor_config)};

    if (!target_ip.isEmpty()) {
      orbbec_config.network.set({.ip_address = target_ip.toStdString()});
    }

    new_device_config = std::move(orbbec_config);
    plugin_prefix = config_label_prefix<OrbbecDeviceConfiguration>;

  } else if (plugin_name == PlyDeviceConfiguration::PluginName) {

    new_device_config = PlyDeviceConfiguration{.id = pc::uuid::word()};
    plugin_prefix = config_label_prefix<PlyDeviceConfiguration>;

  } else {
    pc::logger()->error("Unknown device plugin name '{}'",
                        plugin_name.toStdString());
    return;
  }

  // label the new device after its device type, using the first available
  // number that doesn't clash with other devices in the workspace
  std::visit(
      [&](auto &config) {
        config.label = util::next_available_label(
            plugin_prefix, result_config.devices |
                               std::views::transform(devices::device_label));
      },
      new_device_config);

  result_config.devices.push_back(std::move(new_device_config));

  applyWorkspaceConfigAndRebuild(std::move(result_config),
                                 RebuildScope::Devices);
  emit deviceAdded();
}

void WorkspaceModel::deleteDevice(const QString &device_id) {
  if (_deviceAdapters.isEmpty()) return;

  auto new_config = _workspace.config;

  const auto device_id_str = device_id.toStdString();
  const int device_index = find_device_index_by_id(new_config, device_id_str);

  if (device_index < 0 || device_index >= int(new_config.devices.size())) {
    pc::logger()->warn("deleteSelectedDeviceNode: device id not found '{}'",
                       device_id_str);
    return;
  }

  pc::logger()->trace("Deleting device id='{}' (index {})", device_id_str,
                      device_index);

  const auto device_prefix = node_path_prefix(new_config, device_id_str);
  new_config.devices.erase(new_config.devices.begin() + device_index);
  erase_node_paths(new_config, device_prefix);
  erase_session_node_state(new_config, device_id_str);
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

void WorkspaceModel::deleteSelectedDeviceNode() {
  if (_selectedNodeId.isEmpty()) return;

  auto new_config = _workspace.config;

  const auto node_id = _selectedNodeId.toStdString();

  const int device_index = find_device_index_by_id(new_config, node_id);
  if (device_index < 0) {
    // if we can't find a device with current node id, try deleting a group...
    bool deleted = deleteDeviceGroup(_selectedNodeId);
    if (!deleted) pc::logger()->error("Unable to find device or group by id");
    return;
  }

  pc::logger()->trace("Deleting device '{}' (index {})", node_id, device_index);

  const auto device_prefix = node_path_prefix(new_config, node_id);
  new_config.devices.erase(new_config.devices.begin() + device_index);
  erase_node_paths(new_config, device_prefix);
  erase_session_node_state(new_config, node_id);
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

void WorkspaceModel::duplicateDeviceNode(const QString &node_id) {
  if (node_id.isEmpty()) return;
  const std::string id = node_id.toStdString();

  // TODO the implementation here should really be inside workspace's own
  // source, not the Qt model for workspace!!

  auto new_config = _workspace.config;
  const int device_index = find_device_index_by_id(new_config, id);

  if (device_index >= 0) {
    // if we found this target node id as a device, duplicate that single device
    const int device_index = find_device_index_by_id(new_config, id);
    if (device_index < 0) return;
    auto device_copy = new_config.devices[size_t(device_index)];
    std::visit(
        [](auto &device_config) {
          device_config.id = pc::uuid::word();
          if constexpr (requires { device_config.operators; }) {
            for (auto &operator_config_variant : device_config.operators) {
              std::visit(
                  [](auto &operator_config) {
                    operator_config.id = pc::uuid::word();
                  },
                  operator_config_variant);
            }
          }
        },
        device_copy);
    new_config.devices.push_back(std::move(device_copy));
  } else {
    // if we didn't find it as a device, check if we are targetting a group
    const int group_index = find_device_group_index_by_id(new_config, id);
    if (group_index < 0) {
      pc::logger()->error("Unable to find device node '{}' to delete!", id);
      return;
    }
    // build old_id -> new_id map for the entire subtree (group + descendants)
    std::unordered_map<std::string, std::string> id_map;

    std::function<void(const std::string &)> collect_subtree_ids;
    collect_subtree_ids = [&](const std::string &group_id) {
      id_map[group_id] = pc::uuid::word();

      for (auto &device_group : _workspace.config.device_groups) {
        if (device_group.parent_id.value() == group_id)
          collect_subtree_ids(device_group.id);
      }
      for (auto &device_config : _workspace.config.devices) {
        std::visit(
            [&](const auto &device_config) {
              if constexpr (requires {
                              device_config.id;
                              device_config.parent_id;
                            }) {
                if (device_config.parent_id.value() == group_id)
                  id_map[device_config.id] = pc::uuid::word();
              }
            },
            device_config);
      }
    };

    collect_subtree_ids(id);

    // copy groups in the subtree with remapped ids
    for (const auto &group_config : _workspace.config.device_groups) {
      if (!id_map.count(group_config.id)) continue;
      auto group_copy = group_config;
      group_copy.id = id_map.at(group_config.id);
      const std::string old_parent = group_config.parent_id.value();
      if (id_map.count(old_parent))
        group_copy.parent_id.set(id_map.at(old_parent));
      new_config.device_groups.push_back(std::move(group_copy));
    }

    // copy devices in the subtree with remapped ids and fresh operator ids
    for (auto device_config_variant : _workspace.config.devices) {
      bool in_subtree = false;
      std::visit(
          [&](const auto &device_config) {
            in_subtree = id_map.count(device_config.id) > 0;
          },
          device_config_variant);
      if (!in_subtree) continue;
      std::visit(
          [&](auto &device_config) {
            device_config.id = id_map.at(device_config.id);
            const std::string old_parent = device_config.parent_id.value();
            if (id_map.count(old_parent))
              device_config.parent_id.set(id_map.at(old_parent));
            if constexpr (requires { device_config.operators; }) {
              for (auto &operator_config_variant : device_config.operators) {
                std::visit(
                    [](auto &operator_config) {
                      operator_config.id = pc::uuid::word();
                    },
                    operator_config_variant);
              }
            }
          },
          device_config_variant);
      new_config.devices.push_back(std::move(device_config_variant));
    }
  }

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

void WorkspaceModel::setDeviceLabel(const QString &device_id,
                                    const QString &new_label) {
  const auto id = device_id.toStdString();
  const auto label = new_label.toStdString();

  SetDeviceLabelCommand::State before;
  pc::WorkspaceConfiguration base_snapshot;
  {
    std::scoped_lock lock(_workspace.config_access);
    const int idx = find_device_index_by_id(_workspace.config, id);
    if (idx < 0) return;
    base_snapshot = _workspace.config;
    before.label = std::visit(
        [](const auto &device_config) { return device_config.label.value(); },
        _workspace.config.devices[size_t(idx)]);
    before.publish_paths = _workspace.config.publish_paths.value();
    before.push_paths = _workspace.config.push_paths.value();
  }
  if (before.label == label) return;

  // rename a copy first, so the node's new address can be read back out of it
  const auto previous_prefix = node_path_prefix(base_snapshot, id);
  auto renamed_config = base_snapshot;
  const int idx = find_device_index_by_id(renamed_config, id);
  std::visit([&](auto &device_config) { device_config.label.set(label); },
             renamed_config.devices[size_t(idx)]);
  reroot_node_paths(renamed_config, previous_prefix, id);

  SetDeviceLabelCommand::State after{label,
                                     renamed_config.publish_paths.value(),
                                     renamed_config.push_paths.value()};

  _undoStack->push(new SetDeviceLabelCommand(
      device_id, std::move(before), std::move(after),
      QStringLiteral("Rename device"), std::move(base_snapshot),
      [this](pc::WorkspaceConfiguration config) {
        QMetaObject::invokeMethod(
            this,
            [this, config = std::move(config)]() mutable {
              applyWorkspaceConfigAndRebuild(std::move(config),
                                             RebuildScope::Devices);
            },
            Qt::QueuedConnection);
      }));
}

void WorkspaceModel::duplicateSelectedDeviceNode() {
  duplicateDeviceNode(_selectedNodeId);
}

DeviceAdapter *WorkspaceModel::deviceAdapterAt(int index) const {
  if (index < 0 || index >= _deviceAdapters.size()) return nullptr;
  return qobject_cast<DeviceAdapter *>(_deviceAdapters[index]);
}

QObject *WorkspaceModel::selectedDeviceGroupAdapter() const {
  return _selectedDeviceGroupAdapter.data();
}

void WorkspaceModel::rebuildSelectedGroupAdapter() {
  if (_selectedDeviceGroupAdapter) {
    _selectedDeviceGroupAdapter->deleteLater();
    _selectedDeviceGroupAdapter = nullptr;
  }

  if (_selectedNodeKind == QStringLiteral("group")) {
    const int gi = find_device_group_index_by_id(_workspace.config,
                                                 _selectedNodeId.toStdString());
    if (gi >= 0) {
      // groups have no plugin and no image provider, so pass nullptr for both.
      auto *adapter = new pc::devices::DeviceGroupConfigurationAdapter(
          _workspace.config.device_groups[size_t(gi)], nullptr, nullptr, this);
      adapter->setConfigPath(QString::fromStdString(
          node_path_prefix(_workspace.config, _selectedNodeId.toStdString())));
      watchStreamSourcesFor(adapter);
      initGroupAdapter(adapter);
      _selectedDeviceGroupAdapter = adapter;
    }
  }

  emit selectedDeviceGroupAdapterChanged();
  refreshSelectedGroupHasSequence();
}

void WorkspaceModel::refreshSelectedGroupHasSequence() {
  bool has_sequence = false;

  if (_selectedNodeKind == QStringLiteral("group")) {
    QSet<QString> sequence_device_ids;
    for (QObject *obj : _deviceAdapters) {
      auto *adapter = qobject_cast<DeviceAdapter *>(obj);
      if (!adapter || !adapter->hasSequence()) continue;
      const QString id = adapterStableId(adapter);
      if (!id.isEmpty()) sequence_device_ids.insert(id);
    }

    if (!sequence_device_ids.isEmpty()) {
      // walks nested groups too, so a sequence anywhere beneath counts
      for (const auto &device_id : pc::devices::device_ids_in_group(
               _workspace.config, _selectedNodeId.toStdString())) {
        if (sequence_device_ids.contains(QString::fromStdString(device_id))) {
          has_sequence = true;
          break;
        }
      }
    }
  }

  if (has_sequence == _selectedGroupHasSequence) return;
  _selectedGroupHasSequence = has_sequence;
  emit selectedGroupHasSequenceChanged();
}

void WorkspaceModel::initGroupAdapter(
    pc::devices::DeviceGroupConfigurationAdapter *adapter) {
  if (!adapter) return;

  QObject::connect(
      adapter, &ConfigAdapter::previewRequested, this,
      [this, adapter](const QString &path, const QVariant &value) {
        const QString group_id_q = adapter->id();
        if (group_id_q.isEmpty()) return;

        if (!_groupPreview || _groupPreview->id != group_id_q) {
          std::scoped_lock lock(_workspace.config_access);
          const int group_index = find_device_group_index_by_id(
              _workspace.config, group_id_q.toStdString());
          if (group_index < 0) return;
          _groupPreview.emplace(
              PreviewSession<pc::devices::DeviceGroupConfiguration>{
                  group_id_q,
                  _workspace.config.device_groups[size_t(group_index)],
                  _workspace.config});
        }

        if (!adapter->apply(path, value)) return;
        _groupPreview->dirty = true;

        retransformGroupDescendants(group_id_q.toStdString());
      });

  QObject::connect(
      adapter, &ConfigAdapter::editRequested, this,
      [this, adapter](const QString &path, const QVariant &value) {
        const QString group_id_q = adapter->id();
        if (group_id_q.isEmpty()) return;

        propagateGroupTransport(group_id_q.toStdString(), path, value);

        pc::devices::DeviceGroupConfiguration before;
        pc::devices::DeviceGroupConfiguration after;
        pc::WorkspaceConfiguration base_snapshot;

        const bool closing_preview =
            _groupPreview && _groupPreview->id == group_id_q;
        if (closing_preview) {
          before = _groupPreview->before;
          base_snapshot = _groupPreview->base_snapshot;
        } else {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          const int group_index = find_device_group_index_by_id(
              _workspace.config, group_id_q.toStdString());
          if (group_index < 0) return;
          before = _workspace.config.device_groups[size_t(group_index)];
        }

        // apply mutates the live group config through the adapter's reference.
        const bool changed = adapter->apply(path, value);
        const bool gesture_changed = closing_preview && _groupPreview->dirty;
        if (closing_preview) _groupPreview.reset();
        if (!changed && !gesture_changed) return;

        {
          std::scoped_lock lock(_workspace.config_access);
          const int group_index = find_device_group_index_by_id(
              _workspace.config, group_id_q.toStdString());
          if (group_index < 0) return;
          after = _workspace.config.device_groups[size_t(group_index)];
        }

        const std::string group_id = group_id_q.toStdString();
        QString command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetDeviceGroupConfigCommand(
            group_id_q, std::move(before), std::move(after),
            std::move(command_text), std::move(base_snapshot),
            [this, group_id](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, group_id, config = std::move(config)]() mutable {
                    applyWorkspaceConfigAndRebuild(std::move(config),
                                                   RebuildScope::Devices);
                    retransformGroupDescendants(group_id);
                  },
                  Qt::QueuedConnection);
            }));
      });
}

void WorkspaceModel::retransformGroupDescendants(const std::string &group_id) {
  // orbbec re-applies the world transform every frame, so this only matters for
  // static / sequence PLY devices that won't otherwise re-run apply_transform.
  for (auto &device_plugin : _workspace.devices) {
    if (!device_plugin) continue;
    auto [device_id, _] =
        pc::devices::device_info_from_variant(device_plugin->config_variant());
    if (isDescendantOf(std::string(device_id), group_id))
      device_plugin->on_config_field_changed("transform");
  }
}

void WorkspaceModel::propagateGroupTransport(const std::string &group_id,
                                             const QString &path,
                                             const QVariant &value) {
  const bool set_playing = path == QStringLiteral("sequence/playing");
  const bool set_frame = path == QStringLiteral("sequence/current_frame");
  if (!set_playing && !set_frame) return;

  std::vector<std::string> group_device_ids;
  {
    std::scoped_lock lock(_workspace.config_access);
    group_device_ids =
        pc::devices::device_ids_in_group(_workspace.config, group_id);
  }
  if (group_device_ids.empty()) return;

  for (auto &device_plugin : _workspace.devices) {
    if (!device_plugin || !device_plugin->is_sequence()) continue;
    if (device_plugin->is_discovery_instance()) continue;
    auto [device_id, plugin_name] =
        pc::devices::device_info_from_variant(device_plugin->config_variant());
    if (!std::ranges::contains(group_device_ids, device_id)) continue;

    std::visit(
        [&](auto &device_config) {
          if constexpr (requires { device_config.sequence.value().playing; }) {
            auto &sequence = device_config.sequence.value();
            if (set_playing) {
              sequence.playing.set(value.toBool());
            } else {
              sequence.current_frame.set(value.toInt());
            }
          }
        },
        device_plugin->config());

    device_plugin->on_config_field_changed(path.toStdString());

    // the plugin holds its own config, so mirror the change back into the
    // workspace copy the way a per-device edit would
    std::scoped_lock lock(_workspace.config_access);
    const int index =
        find_device_index_by_id(_workspace.config, std::string(device_id));
    if (index < 0) continue;
    _workspace.config.devices[size_t(index)] = device_plugin->config_variant();
  }
}

void WorkspaceModel::addOperatorToDevice(int deviceIndex,
                                         const QString &operatorPluginName) {
  auto *adapter = deviceAdapterAt(deviceIndex);
  if (!adapter) return;

  const QString device_id = adapterStableId(adapter);
  if (device_id.isEmpty()) return;

  auto new_config = _workspace.config;
  const int idx = find_device_index_by_id(new_config, device_id.toStdString());
  if (idx < 0 || idx >= int(new_config.devices.size())) return;

  const auto op_name = operatorPluginName.toStdString();

  auto *device_operators =
      devices::device_operators(new_config.devices[size_t(idx)]);
  if (!device_operators) {
    pc::logger()->error("Device '{}' cannot host operators",
                        device_id.toStdString());
    return;
  }

  // the new operator is labelled after its operator type, using the first
  // available number that doesn't clash with other operators on the device
  auto new_operator_config = operators::make_operator_config(
      op_name,
      *device_operators | std::views::transform(operators::operator_label));

  if (!new_operator_config.has_value()) {
    pc::logger()->error("Unknown operator plugin name '{}'", op_name);
    return;
  }

  device_operators->push_back(std::move(*new_operator_config));

  // sync workspace config
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);

  // push the updated device variant to the live plugin; sync_operators inside
  // update_config will instantiate the new operator
  if (deviceIndex < int(_workspace.devices.size()) &&
      _workspace.devices[deviceIndex]) {
    auto *p = _workspace.devices[deviceIndex].get();
    pc::logger()->debug(
        "addOperatorToDevice: deviceIndex={} idx={} plugin={} has_workspace={}",
        deviceIndex, idx, fmt::ptr(p), p->has_workspace());
    p->update_config(_workspace.config.devices[size_t(idx)]);
  }
  adapter->rebuildOperatorAdapters();
  attachOperatorConfigAdapters(adapter);
}

void WorkspaceModel::removeOperatorFromDevice(int deviceIndex,
                                              int operatorIndex) {
  auto *adapter = deviceAdapterAt(deviceIndex);
  if (!adapter) return;
  const QString device_id = adapterStableId(adapter);
  if (device_id.isEmpty()) return;
  auto new_config = _workspace.config;
  const int idx = find_device_index_by_id(new_config, device_id.toStdString());
  if (idx < 0 || idx >= int(new_config.devices.size())) return;
  const auto owner_prefix =
      node_path_prefix(new_config, device_id.toStdString());
  std::string operator_prefix;
  std::visit(
      [operatorIndex, &operator_prefix, &owner_prefix](auto &device_config) {
        if constexpr (requires { device_config.operators; }) {
          auto &ops = device_config.operators;
          if (operatorIndex >= 0 && operatorIndex < int(ops.size())) {
            operator_prefix =
                operator_path_prefix(owner_prefix, ops[size_t(operatorIndex)]);
            ops.erase(ops.begin() + operatorIndex);
          }
        }
      },
      new_config.devices[size_t(idx)]);
  if (!operator_prefix.empty()) erase_node_paths(new_config, operator_prefix);
  // sync workspace config
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
  if (deviceIndex < int(_workspace.devices.size()) &&
      _workspace.devices[deviceIndex]) {
    _workspace.devices[deviceIndex]->update_config(
        _workspace.config.devices[size_t(idx)]);
  }
  adapter->rebuildOperatorAdapters();
  attachOperatorConfigAdapters(adapter);
}

void WorkspaceModel::reorderOperatorOnDevice(int deviceIndex, int fromIndex,
                                             int toIndex) {
  if (fromIndex == toIndex) return;
  auto *adapter = deviceAdapterAt(deviceIndex);
  if (!adapter) return;
  const QString device_id = adapterStableId(adapter);
  if (device_id.isEmpty()) return;
  auto new_config = _workspace.config;
  const int idx = find_device_index_by_id(new_config, device_id.toStdString());
  if (idx < 0 || idx >= int(new_config.devices.size())) return;
  std::visit(
      [fromIndex, toIndex](auto &device_config) {
        if constexpr (requires { device_config.operators; }) {
          auto &ops = device_config.operators;
          if (fromIndex < 0 || fromIndex >= int(ops.size())) return;
          if (toIndex < 0 || toIndex >= int(ops.size())) return;
          auto item = std::move(ops[fromIndex]);
          ops.erase(ops.begin() + fromIndex);
          ops.insert(ops.begin() + toIndex, std::move(item));
        }
      },
      new_config.devices[size_t(idx)]);
  // sync workspace config
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::None);
  if (deviceIndex < int(_workspace.devices.size()) &&
      _workspace.devices[deviceIndex]) {
    _workspace.devices[deviceIndex]->update_config(
        _workspace.config.devices[size_t(idx)]);
  }
  adapter->rebuildOperatorAdapters();
  attachOperatorConfigAdapters(adapter);
}

void WorkspaceModel::addOperatorToSession(const QString &sessionId,
                                          const QString &operatorPluginName) {
  auto new_config = _workspace.config;
  const int idx = find_session_index_by_id(new_config, sessionId.toStdString());
  if (idx < 0 || idx >= int(new_config.sessions.size())) return;
  auto &session_config = new_config.sessions[size_t(idx)];
  const auto op_name = operatorPluginName.toStdString();

  // the new operator is labelled after its operator type, using the first
  // available number that doesn't clash with other operators on the session
  auto new_operator_config = operators::make_operator_config(
      op_name, session_config.operators |
                   std::views::transform(operators::operator_label));

  if (!new_operator_config.has_value()) {
    pc::logger()->error("Unknown operator plugin name '{}'", op_name);
    return;
  }

  session_config.operators.push_back(std::move(*new_operator_config));

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Sessions);
}

void WorkspaceModel::removeOperatorFromSession(const QString &sessionId,
                                               int operatorIndex) {
  auto new_config = _workspace.config;
  const int idx = find_session_index_by_id(new_config, sessionId.toStdString());
  if (idx < 0 || idx >= int(new_config.sessions.size())) return;
  auto &ops = new_config.sessions[size_t(idx)].operators;
  if (operatorIndex < 0 || operatorIndex >= int(ops.size())) return;
  const auto owner_prefix =
      session_path_prefix(new_config, sessionId.toStdString());
  const auto operator_prefix =
      operator_path_prefix(owner_prefix, ops[size_t(operatorIndex)]);
  ops.erase(ops.begin() + operatorIndex);
  erase_node_paths(new_config, operator_prefix);
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Sessions);
}

void WorkspaceModel::reorderOperatorOnSession(const QString &sessionId,
                                              int fromIndex, int toIndex) {
  if (fromIndex == toIndex) return;
  auto new_config = _workspace.config;
  const int idx = find_session_index_by_id(new_config, sessionId.toStdString());
  if (idx < 0 || idx >= int(new_config.sessions.size())) return;
  auto &ops = new_config.sessions[size_t(idx)].operators;
  if (fromIndex < 0 || fromIndex >= int(ops.size())) return;
  if (toIndex < 0 || toIndex >= int(ops.size())) return;
  auto item = std::move(ops[size_t(fromIndex)]);
  ops.erase(ops.begin() + fromIndex);
  ops.insert(ops.begin() + toIndex, std::move(item));
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Sessions);
}

void WorkspaceModel::attachOperatorConfigAdapters(
    DeviceAdapter *deviceAdapter) {
  const std::string device_id = adapterStableId(deviceAdapter).toStdString();
  const std::string owner_prefix =
      node_path_prefix(_workspace.config, device_id);
  for (auto *opAdapter : deviceAdapter->operatorAdapters()) {
    auto *plugin = opAdapter->plugin();
    if (!plugin) continue;
    const std::string operator_id = operator_id_of(plugin);
    if (operator_id.empty()) continue;
    auto *storage =
        find_device_operator_config(_workspace.config, device_id, operator_id);
    if (!storage) continue;
    auto *adapter = make_operator_config_adapter(*storage, opAdapter);
    if (adapter) {
      adapter->setConfigPath(
          QString::fromStdString(operator_path_prefix(owner_prefix, *storage)));
      opAdapter->setConfigAdapter(adapter);
      watchStreamSourcesFor(adapter);
      initOperatorAdapter(opAdapter, deviceAdapter);
    }
  }
}

void WorkspaceModel::initOperatorAdapter(OperatorAdapter *opAdapter,
                                         DeviceAdapter *deviceAdapter) {
  auto *innerAdapter = opAdapter->configAdapter();
  if (!innerAdapter) return;

  // a gizmo or a ui input component drag previews continuously and commits once
  // on release, so these land in the workspace config and the running pipeline
  // without touching undo
  QObject::connect(
      innerAdapter, &ConfigAdapter::previewRequested, this,
      [this, opAdapter, deviceAdapter](const QString &path,
                                       const QVariant &value) {
        auto *configAdapter = opAdapter->configAdapter();
        if (!configAdapter) return;
        const QString device_id = adapterStableId(deviceAdapter);
        if (device_id.isEmpty()) return;
        const std::string operator_id_str = operator_id_of(opAdapter->plugin());
        if (operator_id_str.empty()) return;

        const QString preview_id =
            operator_preview_id(device_id, operator_id_str);

        // the first 'preview' that was captured is what undo has to come back
        // to
        if (!_deviceOperatorPreview ||
            _deviceOperatorPreview->id != preview_id) {
          std::scoped_lock lock(_workspace.config_access);
          auto *storage = find_device_operator_config(
              _workspace.config, device_id.toStdString(), operator_id_str);
          if (!storage) return;
          _deviceOperatorPreview.emplace(
              PreviewSession<pc::operators::OperatorConfigurationVariant>{
                  preview_id, *storage, _workspace.config});
        }

        // the adapter aliases the workspace configuration, so this lands there
        if (!configAdapter->apply(path, value)) return;
        _deviceOperatorPreview->dirty = true;

        publish_device_operator_edit(_workspace, opAdapter,
                                     device_id.toStdString(), operator_id_str,
                                     path);
      });

  QObject::connect(
      innerAdapter, &ConfigAdapter::editRequested, this,
      [this, opAdapter, deviceAdapter](const QString &path,
                                       const QVariant &value) {
        // pc::logger()->debug("[op-edit] path='{}' value-typeName='{}' "
        //                     "opAdapter={} deviceAdapter={}",
        //                     path.toStdString(),
        //                     value.typeName() ? value.typeName() : "<null>",
        //                     fmt::ptr(static_cast<void *>(opAdapter)),
        //                     fmt::ptr(static_cast<void *>(deviceAdapter)));
        auto *configAdapter = opAdapter->configAdapter();
        if (!configAdapter) return;
        const QString device_id = adapterStableId(deviceAdapter);
        if (device_id.isEmpty()) return;
        auto *plugin = opAdapter->plugin();
        if (!plugin) return;
        const std::string operator_id_str = operator_id_of(plugin);
        if (operator_id_str.empty()) return;
        const QString operator_id = QString::fromStdString(operator_id_str);

        const QString preview_id =
            operator_preview_id(device_id, operator_id_str);
        const bool closing_preview =
            _deviceOperatorPreview && _deviceOperatorPreview->id == preview_id;

        pc::operators::OperatorConfigurationVariant before;
        pc::WorkspaceConfiguration base_snapshot;
        if (closing_preview) {
          before = _deviceOperatorPreview->before;
          base_snapshot = _deviceOperatorPreview->base_snapshot;
        } else {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          auto *storage = find_device_operator_config(
              _workspace.config, device_id.toStdString(), operator_id_str);
          if (!storage) return;
          before = *storage;
        }

        const bool changed = configAdapter->apply(path, value);
        const bool gesture_changed =
            closing_preview && _deviceOperatorPreview->dirty;
        if (closing_preview) _deviceOperatorPreview.reset();
        if (!changed && !gesture_changed) return;

        auto after = publish_device_operator_edit(_workspace, opAdapter,
                                                  device_id.toStdString(),
                                                  operator_id_str, path);
        if (!after) return;

        QString command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetOperatorConfigCommand(
            device_id, operator_id, std::move(before), std::move(*after),
            std::move(command_text), std::move(base_snapshot),
            [this](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, config = std::move(config)]() mutable {
                    _workspace.apply_new_config(std::move(config), false);
                    refreshOperatorConfigAdapters();
                  },
                  Qt::QueuedConnection);
            }));
      });
}

void WorkspaceModel::attachSessionOperatorConfigAdapters(
    const QString &sessionId, const QList<OperatorAdapter *> &ops) {
  const std::string owner_prefix =
      session_path_prefix(_workspace.config, sessionId.toStdString());
  for (auto *opAdapter : ops) {
    auto *plugin = opAdapter->plugin();
    if (!plugin) continue;
    const std::string operator_id = operator_id_of(plugin);
    if (operator_id.empty()) continue;
    auto *storage = find_session_operator_config(
        _workspace.config, sessionId.toStdString(), operator_id);
    if (!storage) continue;
    auto *adapter = make_operator_config_adapter(*storage, opAdapter);
    if (adapter) {
      const std::string operator_prefix =
          operator_path_prefix(owner_prefix, *storage);
      adapter->setConfigPath(QString::fromStdString(operator_prefix));
      opAdapter->setConfigAdapter(adapter);
      subscribe_adapter_to_registry(*this, adapter, operator_prefix + "/");
      watchStreamSourcesFor(adapter);
      initSessionOperatorAdapter(opAdapter, sessionId);
    }
  }
}

// the configuration editor drops a field's declarative binding the first
// time it writes to that field, so a control only re-reads when
// fieldChanged arrives; swapping the adapter underneath it isn't enough
void WorkspaceModel::notifyOperatorConfigAdapters(
    const QList<OperatorAdapter *> &ops) {
  for (auto *opAdapter : ops) {
    if (!opAdapter) continue;
    if (auto *configAdapter = opAdapter->configAdapter()) {
      configAdapter->notifyAllFieldsChanged();
    }
  }
}

void WorkspaceModel::refreshOperatorConfigAdapters() {
  for (QObject *obj : _deviceAdapters) {
    auto *deviceAdapter = qobject_cast<DeviceAdapter *>(obj);
    if (!deviceAdapter) continue;
    const std::string device_id = adapterStableId(deviceAdapter).toStdString();
    if (device_id.empty()) continue;
    for (auto *opAdapter : deviceAdapter->operatorAdapters()) {
      auto *plugin = opAdapter ? opAdapter->plugin() : nullptr;
      if (!plugin) continue;
      const std::string operator_id = operator_id_of(plugin);
      if (operator_id.empty()) continue;

      pc::operators::OperatorConfigurationVariant config;
      {
        std::scoped_lock lock(_workspace.config_access);
        auto *storage = find_device_operator_config(_workspace.config,
                                                    device_id, operator_id);
        if (!storage) continue;
        config = *storage;
      }
      plugin->update_config(config);
      if (auto *host = opAdapter->host()) {
        host->update_operator_in_pipeline(config, "");
        host->reprocess();
      }
    }
    attachOperatorConfigAdapters(deviceAdapter);
    notifyOperatorConfigAdapters(deviceAdapter->operatorAdapters());
  }

  for (auto it = _sessionOperatorAdapters.begin();
       it != _sessionOperatorAdapters.end(); ++it) {
    const std::string session_id = it.key().toStdString();
    for (auto *opAdapter : it.value()) {
      auto *plugin = opAdapter ? opAdapter->plugin() : nullptr;
      if (!plugin) continue;
      const std::string operator_id = operator_id_of(plugin);
      if (operator_id.empty()) continue;
      std::scoped_lock lock(_workspace.config_access);
      if (auto *storage = find_session_operator_config(
              _workspace.config, session_id, operator_id)) {
        plugin->update_config(*storage);
      }
    }
    attachSessionOperatorConfigAdapters(it.key(), it.value());
    notifyOperatorConfigAdapters(it.value());
  }
}

void WorkspaceModel::initSessionOperatorAdapter(OperatorAdapter *opAdapter,
                                                const QString &sessionId) {
  auto *innerAdapter = opAdapter->configAdapter();
  if (!innerAdapter) return;

  // a gizmo or a ui input component drag previews continuously and commits once
  // on release
  QObject::connect(
      innerAdapter, &ConfigAdapter::previewRequested, this,
      [this, opAdapter, sessionId](const QString &path, const QVariant &value) {
        auto *configAdapter = opAdapter->configAdapter();
        if (!configAdapter) return;
        const std::string operator_id_str = operator_id_of(opAdapter->plugin());
        if (operator_id_str.empty()) return;

        const QString preview_id =
            operator_preview_id(sessionId, operator_id_str);

        // the first 'preview' that was captured is what undo has to come back
        // to
        if (!_sessionOperatorPreview ||
            _sessionOperatorPreview->id != preview_id) {
          std::scoped_lock lock(_workspace.config_access);
          const int s = find_session_index_by_id(_workspace.config,
                                                 sessionId.toStdString());
          if (s < 0) return;
          _sessionOperatorPreview.emplace(PreviewSession<SessionConfiguration>{
              preview_id, _workspace.config.sessions[size_t(s)],
              _workspace.config});
        }

        if (!configAdapter->apply(path, value)) return;
        _sessionOperatorPreview->dirty = true;

        publish_session_operator_edit(_workspace, opAdapter,
                                      sessionId.toStdString(), operator_id_str,
                                      path);
      });

  QObject::connect(
      innerAdapter, &ConfigAdapter::editRequested, this,
      [this, opAdapter, sessionId](const QString &path, const QVariant &value) {
        auto *configAdapter = opAdapter->configAdapter();
        if (!configAdapter) return;
        auto *plugin = opAdapter->plugin();
        if (!plugin) return;
        const std::string operator_id_str = operator_id_of(plugin);
        if (operator_id_str.empty()) return;

        const QString preview_id =
            operator_preview_id(sessionId, operator_id_str);
        const bool closing_preview = _sessionOperatorPreview &&
                                     _sessionOperatorPreview->id == preview_id;

        SessionConfiguration before;
        SessionConfiguration after;
        pc::WorkspaceConfiguration base_snapshot;
        if (closing_preview) {
          before = _sessionOperatorPreview->before;
          base_snapshot = _sessionOperatorPreview->base_snapshot;
        } else {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          const int s = find_session_index_by_id(_workspace.config,
                                                 sessionId.toStdString());
          if (s < 0) return;
          before = _workspace.config.sessions[size_t(s)];
        }

        // the adapter aliases the workspace configuration, so this lands there
        const bool changed = configAdapter->apply(path, value);
        const bool gesture_changed =
            closing_preview && _sessionOperatorPreview->dirty;
        if (closing_preview) _sessionOperatorPreview.reset();
        if (!changed && !gesture_changed) return;

        {
          std::scoped_lock lock(_workspace.config_access);
          const int s = find_session_index_by_id(_workspace.config,
                                                 sessionId.toStdString());
          if (s < 0) return;
          after = _workspace.config.sessions[size_t(s)];
        }

        publish_session_operator_edit(_workspace, opAdapter,
                                      sessionId.toStdString(), operator_id_str,
                                      path);

        QString command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetSessionConfigCommand(
            sessionId, std::move(before), std::move(after),
            std::move(command_text), std::move(base_snapshot),
            [this](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, config = std::move(config)]() mutable {
                    // update config + sessions, don't rebuild devices
                    _workspace.apply_new_config(std::move(config), false);
                    syncSessionAdapters();
                    refreshOperatorConfigAdapters();
                  },
                  Qt::QueuedConnection);
            }));
      });
}

pc::ConfigRegistry::SubscriptionId
WorkspaceModel::subscribeToRegistry(std::string prefix,
                                    pc::ConfigRegistry::ChangeCallback cb) {
  const auto id = _workspace.config_registry.on_change(prefix, std::move(cb));
  _registrySubscriptions.emplace_back(id, std::move(prefix));
  return id;
}

void WorkspaceModel::unsubscribeFromRegistry(std::string_view area) {
  std::erase_if(_registrySubscriptions, [&](const auto &sub) {
    const auto &[id, prefix] = sub;
    if (!prefix.starts_with(area)) return false;
    _workspace.config_registry.remove_subscription(id);
    return true;
  });
}

void WorkspaceModel::initPointStreamerAdapter() {
  if (!_pointStreamerAdapter) return;

  // Subscribe to external (OSC/etc.) changes on the streamer config
  unsubscribeFromRegistry("streaming/");
  auto adapterPtr = QPointer<ConfigAdapter>(_pointStreamerAdapter.data());
  subscribeToRegistry(
      "streaming/", [adapterPtr](std::string_view path) {
        if (!adapterPtr) return;
        const std::string local(
            path.substr(std::string_view("streaming/").size()));
        const QString qpath = QString::fromStdString(local);
        QMetaObject::invokeMethod(
            adapterPtr.data(),
            [adapterPtr, qpath]() {
              if (adapterPtr) adapterPtr->notifyFieldChanged(qpath);
            },
            Qt::QueuedConnection);
      });

  QObject::connect(
      _pointStreamerAdapter, &ConfigAdapter::editRequested, this,
      [this](const QString &path, const QVariant &value) {
        auto *adapter = _pointStreamerAdapter.data();
        if (!adapter) return;

        pc::networking::PointStreamerConfiguration before;
        pc::networking::PointStreamerConfiguration after;
        pc::WorkspaceConfiguration base_snapshot;
        {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          before = _workspace.config.point_streamer.value();
          // apply() mutates the live config member directly (m_config aliases
          // it)
          const bool changed = adapter->apply(path, value);
          if (!changed) return;
          after = _workspace.config.point_streamer.value();

          if (path == QStringLiteral("codec_config/codec")) {
            using Codec = pc::CodecConfiguration;
            auto &compression =
                after.codec_config.value().compression.value().variant();
            switch (value.toInt()) {
            case 0: compression = Codec::NoCompressionConfiguration{}; break;
            case 1: compression = Codec::MeshoptCompressionConfiguration{}; break;
            case 2: compression = Codec::DracoCompressionConfiguration{}; break;
            default: break;
            }
          }
        }

        QString command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetPointStreamerConfigCommand(
            std::move(before), std::move(after), std::move(command_text),
            std::move(base_snapshot),
            [this](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, config = std::move(config)]() mutable {
                    // the adapter's reference points into the live config, so
                    // it has to be handed the incoming value while it still
                    // holds the old one. diffing after apply_new_config
                    // compares the new value against itself and reports
                    // nothing changed
                    if (_pointStreamerAdapter) {
                      std::scoped_lock lock(_workspace.config_access);
                      _pointStreamerAdapter->setConfig(pc::ConfigurationVariant{
                          config.point_streamer.value()});
                    }
                    _workspace.apply_new_config(std::move(config), false);
                  },
                  Qt::QueuedConnection);
            }));
      });
}

void WorkspaceModel::initPublishersConfigAdapter() {
  if (!_publishersConfigAdapter) return;

  // Subscribe to external (OSC/etc.) changes on the publishers config
  unsubscribeFromRegistry("publishers/");
  auto adapterPtr = QPointer<ConfigAdapter>(_publishersConfigAdapter.data());
  subscribeToRegistry(
      "publishers/", [adapterPtr](std::string_view path) {
        if (!adapterPtr) return;
        const std::string local(
            path.substr(std::string_view("publishers/").size()));
        const QString qpath = QString::fromStdString(local);
        QMetaObject::invokeMethod(
            adapterPtr.data(),
            [adapterPtr, qpath]() {
              if (adapterPtr) adapterPtr->notifyFieldChanged(qpath);
            },
            Qt::QueuedConnection);
      });

  QObject::connect(
      _publishersConfigAdapter, &ConfigAdapter::editRequested, this,
      [this](const QString &path, const QVariant &value) {
        auto *adapter = _publishersConfigAdapter.data();
        if (!adapter) return;

        pc::publishers::PublishersConfiguration before;
        pc::publishers::PublishersConfiguration after;
        pc::WorkspaceConfiguration base_snapshot;
        {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          before = _workspace.config.publishers.value();
          // apply() mutates the live config member directly (m_config aliases
          // it)
          const bool changed = adapter->apply(path, value);
          if (!changed) return;
          after = _workspace.config.publishers.value();
        }

        // apply() writes the config directly rather than going through
        // config_registry.set(), so tell subscribers ourselves
        _workspace.config_registry.notify("publishers/" + path.toStdString());

        QString command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetPublishersConfigCommand(
            std::move(before), std::move(after), std::move(command_text),
            std::move(base_snapshot),
            [this](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, config = std::move(config)]() mutable {
                    // as above: the adapter has to diff against the value it
                    // still holds, before the live config is overwritten
                    if (_publishersConfigAdapter) {
                      std::scoped_lock lock(_workspace.config_access);
                      _publishersConfigAdapter->setConfig(
                          pc::ConfigurationVariant{config.publishers.value()});
                    }
                    _workspace.apply_new_config(std::move(config), false);
                  },
                  Qt::QueuedConnection);
            }));
      });
}

void WorkspaceModel::rebuildSessionOperatorAdapters(
    const QString &sessionId, SessionConfigurationAdapter *adapter,
    pc::Session *session) {
  auto &list = _sessionOperatorAdapters[sessionId];
  qDeleteAll(list);
  list.clear();
  if (session) {
    for (auto &op : session->operators) {
      if (!op) continue;
      auto *opAdapter = new OperatorAdapter(op.get(), session, adapter);
      list.append(opAdapter);
    }
  }
  attachSessionOperatorConfigAdapters(sessionId, list);
}

void WorkspaceModel::syncSessionPointCloudAdapters() {
  // Sync session point cloud adapters (one per live session).
  // Same callback pattern as device adapters.
  QSet<QString> live_session_ids;
  for (auto &[session_id_str, session_ptr] : _workspace.sessions) {
    const QString id = QString::fromStdString(session_id_str);
    live_session_ids.insert(id);

    if (!_sessionPointCloudAdapters.contains(id) ||
        !_sessionPointCloudAdapters[id]) {
      auto *adapter =
          new SessionAdapter(session_ptr.get(), _imageProvider, this);
      _sessionPointCloudAdapters[id] = adapter;
    }

    // Re-wire the callback every sync so a rebuilt session gets a fresh hook.
    auto gate = std::make_shared<SessionUpdateGate>();
    gate->adapter = QPointer<SessionAdapter>(_sessionPointCloudAdapters[id]);
    auto *model = this; // outlives all sessions; safe to capture raw

    session_ptr->set_point_cloud_updated_callback([gate, model]() {
      // Called on a pipeline worker thread... but if an update is already
      // queued for the UI thread, just drop this one
      bool expected = false;
      if (!gate->pending.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel))
        return;
      QMetaObject::invokeMethod(
          model,
          [gate]() {
            gate->pending.store(false, std::memory_order_release);
            if (gate->adapter) gate->adapter->notifyPointCloudUpdated();
          },
          Qt::QueuedConnection);
    });

    auto pbGate = std::make_shared<SessionUpdateGate>();
    pbGate->adapter = QPointer<SessionAdapter>(_sessionPointCloudAdapters[id]);

    session_ptr->set_playback_changed_callback([pbGate, model]() {
      bool expected = false;
      if (!pbGate->pending.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel))
        return;
      QMetaObject::invokeMethod(
          model,
          [pbGate]() {
            pbGate->pending.store(false, std::memory_order_release);
            if (pbGate->adapter) pbGate->adapter->notifyPlaybackChanged();
          },
          Qt::QueuedConnection);
    });
  }
  // Remove adapters for sessions that no longer exist
  for (auto it = _sessionPointCloudAdapters.begin();
       it != _sessionPointCloudAdapters.end();) {
    if (!live_session_ids.contains(it.key())) {
      if (it.value()) it.value()->deleteLater();
      it = _sessionPointCloudAdapters.erase(it);
    } else {
      ++it;
    }
  }

  emit selectedOperatorFrameSourceChanged();
}

SessionAdapter *
WorkspaceModel::sessionPointCloudAdapterFor(const QString &sessionId) const {
  auto it = _sessionPointCloudAdapters.find(sessionId);
  return (it != _sessionPointCloudAdapters.end() && it.value())
             ? it.value().data()
             : nullptr;
}

void WorkspaceModel::syncSessionOperatorAdapters() {
  bool operators_changed = false;
  QStringList rebuilt_session_ids;
  QSet<QString> live_ids;
  for (QObject *obj : _sessionAdapters) {
    auto *adapter = qobject_cast<SessionConfigurationAdapter *>(obj);
    if (!adapter) continue;
    const QString id = adapter->id();
    if (id.isEmpty()) continue;
    live_ids.insert(id);

    pc::Session *session = nullptr;
    {
      auto it = _workspace.sessions.find(id.toStdString());
      if (it != _workspace.sessions.end()) session = it->second.get();
    }

    const bool has_entry = _sessionOperatorAdapters.contains(id);
    const auto &current = _sessionOperatorAdapters[id];
    if (!has_entry || session_operator_structure_changed(current, session)) {
      rebuildSessionOperatorAdapters(id, adapter, session);
      rebuilt_session_ids.append(id);
      operators_changed = true;
    } else if (operator_config_storage_moved(
                   current, [&](const std::string &operator_id) {
                     return find_session_operator_config(
                         _workspace.config, id.toStdString(), operator_id);
                   })) {
      // structure is intact but the session config storage moved underneath
      attachSessionOperatorConfigAdapters(id, current);
    }

    resubscribe_operator_adapters(*this, _sessionOperatorAdapters[id]);
  }

  // drop operator adapters for sessions that no longer exist; they were
  // parented to their (now-deleted) session adapter, so just drop references
  for (auto it = _sessionOperatorAdapters.begin();
       it != _sessionOperatorAdapters.end();) {
    if (!live_ids.contains(it.key())) {
      it = _sessionOperatorAdapters.erase(it);
      operators_changed = true;
    } else {
      ++it;
    }
  }

  for (const auto &session_id : rebuilt_session_ids)
    emit sessionOperatorsChanged(session_id);

  if (operators_changed) emit selectedSessionChanged();
}

void WorkspaceModel::setSelectedOperatorAdapter(OperatorAdapter *adapter) {
  if (_selectedOperatorAdapter == adapter) return;
  if (_selectedOperatorAdapter) {
    disconnect(_selectedOperatorAdapter, &QObject::destroyed, this, nullptr);
  }
  _selectedOperatorAdapter = adapter;
  if (adapter) {
    connect(adapter, &QObject::destroyed, this, [this] {
      // tell QML when adapters are destroyed
      emit selectedOperatorAdapterChanged();
      emit selectedOperatorFrameSourceChanged();
    });
  }
  emit selectedOperatorAdapterChanged();
  emit selectedOperatorFrameSourceChanged();
}

QObject *WorkspaceModel::selectedOperatorFrameSource() const {
  if (!_selectedOperatorAdapter) return nullptr;
  auto *host = _selectedOperatorAdapter->host();
  if (!host) return nullptr;

  for (QObject *obj : _deviceAdapters) {
    auto *device_adapter = qobject_cast<DeviceAdapter *>(obj);
    if (!device_adapter) continue;
    if (static_cast<pc::operators::OperatorHost *>(device_adapter->plugin()) ==
        host) {
      return device_adapter;
    }
  }

  for (const auto &session_adapter : _sessionPointCloudAdapters) {
    if (!session_adapter) continue;
    if (static_cast<pc::operators::OperatorHost *>(
            session_adapter->session()) == host) {
      return session_adapter.data();
    }
  }

  return nullptr;
}

// TODO whats the change here?
void WorkspaceModel::initSessionAdapter(SessionConfigurationAdapter *adapter) {
  if (!adapter) return;
  QObject::connect(adapter, &ConfigAdapter::editRequested, this,
                   [this, adapter](const QString &path, const QVariant &value) {
                     auto *session_adapter =
                         qobject_cast<SessionConfigurationAdapter *>(adapter);
                     if (!session_adapter) return;
                     const QString session_id_q = session_adapter->id();
                     if (session_id_q.isEmpty()) return;
                     SessionConfiguration before;
                     SessionConfiguration after;
                     pc::WorkspaceConfiguration base_snapshot;
                     QString command_text;
                     {
                       std::scoped_lock lock(_workspace.config_access);
                       base_snapshot = _workspace.config;
                       const int idx = find_session_index_by_id(
                           _workspace.config, session_id_q.toStdString());
                       if (idx < 0) return;
                       before = _workspace.config.sessions[size_t(idx)];
                       const bool changed = session_adapter->apply(path, value);
                       if (!changed) return;
                       after = _workspace.config.sessions[size_t(idx)];
                     }
                     command_text = QStringLiteral("Edit %1").arg(path);
                     _undoStack->push(new SetSessionConfigCommand(
                         session_id_q, std::move(before), std::move(after),
                         std::move(command_text), std::move(base_snapshot),
                         [this](pc::WorkspaceConfiguration config) {
                           QMetaObject::invokeMethod(
                               this,
                               [this, config = std::move(config)]() mutable {
                                 // only update config, don't rebuild devices
                                 _workspace.apply_new_config(std::move(config),
                                                             false);
                                 syncSessionAdapters();
                               },
                               Qt::QueuedConnection);
                         }));
                   });
}

template <typename AdapterT>
void WorkspaceModel::initDeviceAdapter(AdapterT *adapter,
                                       pc::devices::DevicePlugin *plugin) {
  using ConfigT = typename AdapterT::config_type;
  if constexpr (std::same_as<ConfigT, devices::DeviceGroupConfiguration>) {
    return;
  }
  const auto &[device_id, plugin_name] =
      devices::device_info_from_variant(plugin->config_variant());
  pc::logger()->trace("Initialising device adapter '{}' for plugin '{}'",
                      device_id, plugin_name);
  QObject::connect(
      adapter, &ConfigAdapter::previewRequested, this,
      [this, adapter](const QString &path, const QVariant &value) {
        auto *device_adapter = qobject_cast<DeviceAdapter *>(adapter);
        if (!device_adapter) return;

        const QString device_id_q = adapterStableId(device_adapter);
        if (device_id_q.isEmpty()) return;

        // the first 'preview' that was captured is what undo has to come back
        // to
        if (!_devicePreview || _devicePreview->id != device_id_q) {
          std::scoped_lock lock(_workspace.config_access);
          const int idx = find_device_index_by_id(_workspace.config,
                                                  device_id_q.toStdString());
          if (idx < 0) return;
          _devicePreview.emplace(
              PreviewSession<pc::devices::DeviceConfigurationVariant>{
                  device_id_q, _workspace.config.devices[size_t(idx)],
                  _workspace.config});
        }

        if (!device_adapter->apply(path, value)) return;
        auto *p = device_adapter->plugin();
        if (!p) return;

        _devicePreview->dirty = true;
        std::scoped_lock lock(_workspace.config_access);
        const int idx = find_device_index_by_id(_workspace.config,
                                                device_id_q.toStdString());
        if (idx < 0) return;
        _workspace.config.devices[size_t(idx)] = p->config_variant();
      });

  QObject::connect(
      adapter, &ConfigAdapter::editRequested, this,
      [this, adapter](const QString &path, const QVariant &value) {
        auto *device_adapter = qobject_cast<DeviceAdapter *>(adapter);
        if (!device_adapter) return;

        const QString device_id_q = adapterStableId(device_adapter);
        if (device_id_q.isEmpty()) return;

        pc::devices::DeviceConfigurationVariant before;
        pc::devices::DeviceConfigurationVariant after;
        pc::WorkspaceConfiguration base_snapshot;
        QString command_text;

        const bool closing_preview =
            _devicePreview && _devicePreview->id == device_id_q;
        if (closing_preview) {
          before = _devicePreview->before;
          base_snapshot = _devicePreview->base_snapshot;
        } else {
          std::scoped_lock lock(_workspace.config_access);
          base_snapshot = _workspace.config;
          const int idx = find_device_index_by_id(_workspace.config,
                                                  device_id_q.toStdString());
          if (idx < 0) return;
          before = _workspace.config.devices[size_t(idx)];
        }

        const bool changed = device_adapter->apply(path, value);
        // a commit landing on the last previewed value changes nothing here,
        // but the gesture as a whole still has to be recorded
        const bool gesture_changed = closing_preview && _devicePreview->dirty;
        if (closing_preview) _devicePreview.reset();
        if (!changed && !gesture_changed) return;
        auto *p = device_adapter->plugin();
        if (!p) return;
        after = p->config_variant();
        {
          std::scoped_lock lock(_workspace.config_access);
          const int idx = find_device_index_by_id(_workspace.config,
                                                  device_id_q.toStdString());
          if (idx < 0) return;
          _workspace.config.devices[size_t(idx)] = after;
        }
        command_text = QStringLiteral("Edit %1").arg(path);
        _undoStack->push(new SetDeviceConfigCommand(
            device_id_q, std::move(before), std::move(after),
            std::move(command_text), std::move(base_snapshot),
            [this](pc::WorkspaceConfiguration config) {
              QMetaObject::invokeMethod(
                  this,
                  [this, config = std::move(config)]() mutable {
                    applyWorkspaceConfigAndRebuild(std::move(config),
                                                   RebuildScope::Devices);
                  },
                  Qt::QueuedConnection);
            }));
      });

  plugin->set_status_callback([adapterPtr = QPointer<AdapterT>(adapter)](
                                  pc::devices::DeviceStatus status) {
    if (!adapterPtr) return;
    QMetaObject::invokeMethod(
        adapterPtr.data(),
        [adapterPtr, status]() {
          if (!adapterPtr) return;
          adapterPtr->setStatusFromCore(status);
        },
        Qt::QueuedConnection);
  });
}

DeviceAdapter *WorkspaceModel::makeDeviceAdapterForPlugin(
    pc::devices::DevicePlugin *plugin,
    pc::devices::DeviceConfigurationVariant &config_variant) {
  DeviceAdapter *result = nullptr;
  std::visit(
      [this, plugin, &result](auto &device_config) {
        using ConfigType = std::decay_t<decltype(device_config)>;
        if constexpr (!std::same_as<ConfigType,
                                    pc::devices::DeviceGroupConfiguration>) {
          using AdapterType =
              pc::devices::device_adapter_for_config_t<ConfigType>;
          auto *adapter =
              new AdapterType(device_config, plugin, _imageProvider, this);
          initDeviceAdapter(adapter, plugin);
          result = adapter;
        }
      },
      config_variant);
  return result;
}

void WorkspaceModel::syncAdapters() {
  syncSessionAdapters();
  syncDeviceAdapters();
}

void WorkspaceModel::syncSessionAdapters() {
  pc::logger()->trace("Syncing session adapters");
  // config.sessions may have just reallocated, so the adapters cannot be asked
  // for their own ids
  auto existing_by_id = _sessionAdapterById;

  QList<QObject *> new_ordered_sessions;
  QHash<QString, const pc::SessionConfiguration *> new_ptrs_by_id;
  QHash<QString, QPointer<SessionConfigurationAdapter>> new_adapters_by_id;
  QHash<QString, QString> new_labels_by_id;
  {
    std::scoped_lock lock(_workspace.config_access);

    new_ordered_sessions.reserve(int(_workspace.config.sessions.size()));
    new_ptrs_by_id.reserve(int(_workspace.config.sessions.size()));

    for (auto &session_config : _workspace.config.sessions) {
      const QString id = QString::fromStdString(session_config.id);
      if (id.isEmpty()) continue;
      const pc::SessionConfiguration *current_ptr = &session_config;
      SessionConfigurationAdapter *adapter = existing_by_id.take(id).data();
      const bool ptr_matches = _sessionConfigPtrById.contains(id) &&
                               (_sessionConfigPtrById.value(id) == current_ptr);
      if (adapter && !ptr_matches) {
        // Underlying SessionConfiguration moved (vector realloc / reorder).
        // Old adapter holds a dangling reference: must recreate. Its operator
        // adapters are children and will be deleted with it; drop our refs.
        adapter->deleteLater();
        adapter = nullptr;
        _sessionOperatorAdapters.remove(id);
      }
      if (adapter) {
        adapter->setConfig(session_config);
        adapter->notifyAllFieldsChanged();
      } else {
        auto *new_adapter =
            new SessionConfigurationAdapter(session_config, this);
        initSessionAdapter(new_adapter);
        adapter = new_adapter;
      }
      if (adapter) {
        new_ordered_sessions.append(adapter);
        new_ptrs_by_id.insert(id, current_ptr);
        new_adapters_by_id.insert(id, adapter);
        new_labels_by_id.insert(
            id, QString::fromStdString(session_config.label.value()));
      }
    }
  }

  // Delete adapters for removed sessions (anything left in existing_by_id).
  for (auto it = existing_by_id.begin(); it != existing_by_id.end(); ++it) {
    if (it.value()) it.value()->deleteLater();
  }

  const bool adapters_changed = _sessionAdapters != new_ordered_sessions ||
                                _sessionLabelById != new_labels_by_id;
  _sessionAdapters = new_ordered_sessions;
  _sessionConfigPtrById = std::move(new_ptrs_by_id);
  _sessionAdapterById = std::move(new_adapters_by_id);
  _sessionLabelById = std::move(new_labels_by_id);

  // the selection has to name a session that still exists
  {
    const auto live_session_ids = _sessionAdapterById.keys();
    QString desired_session_id = _selectedSessionId;
    if (!live_session_ids.contains(desired_session_id)) {
      desired_session_id =
          QString::fromStdString(_workspace.config.selectedSessionId.value());
    }
    if (!live_session_ids.contains(desired_session_id) &&
        !_workspace.config.sessions.empty()) {
      desired_session_id =
          QString::fromStdString(_workspace.config.sessions.front().id);
    }
    if (desired_session_id != _selectedSessionId) {
      _selectedSessionId = desired_session_id;
      _workspace.config.selectedSessionId.set(desired_session_id.toStdString());
      emit selectedSessionChanged();
      emit deviceTreeRowsChanged();
    }
  }

  // Refresh registry subscriptions so OSC/external changes notify QML.
  unsubscribeFromRegistry("session/");
  for (QObject *obj : _sessionAdapters) {
    auto *adapter = qobject_cast<ConfigAdapter *>(obj);
    if (!adapter) continue;
    const std::string id = adapterStableId(adapter).toStdString();
    if (id.empty()) continue;
    const std::string node_prefix = session_path_prefix(_workspace.config, id);
    adapter->setConfigPath(QString::fromStdString(node_prefix));
    set_nested_config_path(adapter, node_prefix, "operator_pipeline");
    const std::string prefix = node_prefix + "/";
    auto adapterPtr = QPointer<ConfigAdapter>(adapter);
    subscribeToRegistry(
        prefix, [adapterPtr, prefix](std::string_view path) {
          if (!adapterPtr) return;
          std::string local(path.substr(prefix.size()));
          // collapse float3 component sub-paths to the parent path
          // e.g. "transform/position/x" becomes "transform/position"
          for (std::string_view component_suffix : {"/x", "/y", "/z"}) {
            if (local.ends_with(component_suffix)) {
              local = local.substr(0, local.size() - component_suffix.size());
              break;
            }
          }
          const QString qpath = QString::fromStdString(local);
          QMetaObject::invokeMethod(
              adapterPtr.data(),
              [adapterPtr, qpath]() {
                if (adapterPtr) adapterPtr->notifyFieldChanged(qpath);
              },
              Qt::QueuedConnection);
        });
  }

  if (adapters_changed) emit sessionAdaptersChanged();

  syncSessionPointCloudAdapters();

  // Rebuild per-session operator adapters from each session's live
  // OperatorPlugin instances.
  syncSessionOperatorAdapters();

  pc::logger()->trace("Finished syncing sessionAdapters");
}

void WorkspaceModel::syncDeviceAdapters() {
  pc::logger()->trace("syncDeviceAdapters: begin, workspace device count={}",
                      _workspace.devices.size());

  QHash<QString, DeviceAdapter *> existing_by_id;
  existing_by_id.reserve(_deviceAdapters.size());
  for (QObject *obj : _deviceAdapters) {
    auto *a = qobject_cast<DeviceAdapter *>(obj);
    if (!a) continue;
    const QString id = adapterStableId(a);
    if (!id.isEmpty()) existing_by_id.insert(id, a);
  }
  pc::logger()->trace("syncDeviceAdapters: existing adapter count={}",
                      existing_by_id.size());

  QList<QObject *> new_ordered_devices;
  new_ordered_devices.reserve(_deviceAdapters.size());

  for (auto &device_plugin : _workspace.devices) {
    auto *plugin = device_plugin.get();
    DeviceAdapter *adapter = nullptr;
    using DeviceConfigVariantRef =
        std::reference_wrapper<devices::DeviceConfigurationVariant>;
    std::optional<DeviceConfigVariantRef> device_config;
    if (plugin) device_config = plugin->config_variant();

    if (device_config.has_value()) {
      auto config = device_config.value();
      auto [device_id, plugin_name] = devices::device_info_from_variant(config);
      const QString id(device_id.data());
      pc::logger()->trace(
          "syncDeviceAdapters: processing device id='{}' plugin='{}'",
          device_id, plugin_name);

      if (!id.isEmpty()) {
        adapter = existing_by_id.take(id);
        if (adapter) {
          pc::logger()->trace(
              "syncDeviceAdapters: found existing adapter for id='{}'",
              device_id);
          bool sync_success = false;
          try {
            sync_success = adapter->setConfig(config);
          } catch (...) {
            pc::logger()->error(
                "syncDeviceAdapters: exception setting config for '{}' '{}'",
                plugin_name, device_id);
          }
          if (!sync_success) {
            pc::logger()->trace("syncDeviceAdapters: setConfig failed for "
                                "id='{}', recreating adapter",
                                device_id);
            adapter->deleteLater();
            adapter = nullptr;
          } else {
            pc::logger()->trace("syncDeviceAdapters: setConfig ok for id='{}'",
                                device_id);
            adapter->notifyAllFieldsChanged();
          }
        } else {
          pc::logger()->trace("syncDeviceAdapters: no existing adapter for "
                              "id='{}', will construct",
                              device_id);
        }
      }

      if (!adapter) {
        pc::logger()->trace("syncDeviceAdapters: constructing new adapter for "
                            "id='{}' plugin='{}'",
                            device_id, plugin_name);
        adapter = makeDeviceAdapterForPlugin(plugin, config);
        pc::logger()->trace("syncDeviceAdapters: construction {}",
                            adapter ? "ok" : "FAILED");
      }

      if (adapter) {
        adapter->setDeviceId(id);
        pc::logger()->trace("syncDeviceAdapters: hooking callbacks for id='{}'",
                            device_id);
        auto adapterPtr = QPointer<DeviceAdapter>(adapter);
        plugin->set_point_cloud_updated_callback([adapterPtr]() {
          QMetaObject::invokeMethod(adapterPtr.data(), [adapterPtr]() {
            if (!adapterPtr) {
              pc::logger()->error("Invalid point cloud plugin ptr");
              return;
            }
            adapterPtr->notifyPointCloudUpdated();
            adapterPtr->syncCameraFrames();
          });
        });

        plugin->set_status_callback(
            [adapterPtr](pc::devices::DeviceStatus status) {
              if (!adapterPtr) return;
              QMetaObject::invokeMethod(
                  adapterPtr.data(),
                  [adapterPtr, status]() {
                    if (!adapterPtr) return;
                    adapterPtr->setStatusFromCore(status);
                  },
                  Qt::QueuedConnection);
            });

        // a sequence only shows up once the device has loaded one, so the
        // group timeline's visibility has to track it as it appears/vanishes
        QObject::connect(adapter, &DeviceAdapter::sequenceStateChanged, this,
                         &WorkspaceModel::refreshSelectedGroupHasSequence,
                         Qt::UniqueConnection);

        const bool structure_changed =
            operator_structure_changed(adapter, plugin);
        if (structure_changed) {
          pc::logger()->trace("syncDeviceAdapters: operator structure changed "
                              "for id='{}', rebuilding",
                              device_id);
          adapter->rebuildOperatorAdapters();
        }
        // an unchanged operator structure still leaves the config adapters of
        // a reused device adapter pointing at the previous workspace config
        const std::string device_id_str(device_id);
        if (structure_changed ||
            operator_config_storage_moved(adapter->operatorAdapters(),
                                          [&](const std::string &operator_id) {
                                            return find_device_operator_config(
                                                _workspace.config,
                                                device_id_str, operator_id);
                                          })) {
          pc::logger()->trace("syncDeviceAdapters: rebinding operator config "
                              "adapters for id='{}'",
                              device_id);
          attachOperatorConfigAdapters(adapter);
        }
      }
    } else {
      pc::logger()->trace(
          "syncDeviceAdapters: plugin has no config variant, skipping");
    }

    if (adapter) {
      new_ordered_devices.append(adapter);
      pc::logger()->trace("syncDeviceAdapters: appended adapter, list size={}",
                          new_ordered_devices.size());
    } else {
      pc::logger()->error(
          "syncDeviceAdapters: no adapter produced for a device plugin");
    }
  }

  pc::logger()->trace("syncDeviceAdapters: deleting {} stale adapters",
                      existing_by_id.size());
  for (auto it = existing_by_id.begin(); it != existing_by_id.end(); ++it) {
    if (it.value()) {
      pc::logger()->trace("syncDeviceAdapters: deleting stale adapter id='{}'",
                          it.key().toStdString());
      it.value()->deleteLater();
    }
  }

  _deviceAdapters = new_ordered_devices;
  pc::logger()->trace("syncDeviceAdapters: device adapters rebuilt, count={}",
                      _deviceAdapters.size());

  unsubscribeFromRegistry("device/");
  pc::logger()->trace(
      "syncDeviceAdapters: removed existing device/ registry subscriptions");

  for (QObject *obj : _deviceAdapters) {
    auto *adapter = qobject_cast<ConfigAdapter *>(obj);
    if (!adapter) continue;
    const std::string id = adapterStableId(adapter).toStdString();
    if (id.empty()) continue;
    const std::string node_prefix = node_path_prefix(_workspace.config, id);
    adapter->setConfigPath(QString::fromStdString(node_prefix));
    set_nested_config_path(adapter, node_prefix, "operator_pipeline");
    watchStreamSourcesFor(adapter);
    const std::string prefix = node_prefix + "/";
    pc::logger()->trace(
        "syncDeviceAdapters: registering on_change for prefix='{}'", prefix);
    auto adapterPtr = QPointer<ConfigAdapter>(adapter);
    subscribeToRegistry(
        prefix, [adapterPtr, prefix, id,
                 &workspace = _workspace](std::string_view path) {
          if (!workspace.config_registry.is_readonly(path)) {
            std::scoped_lock lock(workspace.config_access);
            for (auto &device_plugin : workspace.devices) {
              if (!device_plugin) continue;
              auto [device_id, _] = pc::devices::device_info_from_variant(
                  device_plugin->config_variant());
              if (std::string(device_id) != id) continue;
              for (auto &wc : workspace.config.devices) {
                const bool match =
                    std::visit([&id](const auto &d) { return d.id == id; }, wc);
                if (match) {
                  wc = device_plugin->config_variant();
                  break;
                }
              }
              break;
            }
          }
          if (!adapterPtr) return;
          std::string local(path.substr(prefix.size()));
          // the node prefix on its own means the whole config was replaced
          // underneath the adapter, so every field has to be read
          if (local.empty()) {
            QMetaObject::invokeMethod(
                adapterPtr.data(),
                [adapterPtr]() {
                  if (adapterPtr) adapterPtr->notifyAllFieldsChanged();
                },
                Qt::QueuedConnection);
            return;
          }
          for (std::string_view component_suffix : {"/x", "/y", "/z"}) {
            if (local.ends_with(component_suffix)) {
              local = local.substr(0, local.size() - component_suffix.size());
              break;
            }
          }
          const QString qpath = QString::fromStdString(local);
          QMetaObject::invokeMethod(
              adapterPtr.data(),
              [adapterPtr, qpath]() {
                if (adapterPtr) adapterPtr->notifyFieldChanged(qpath);
              },
              Qt::QueuedConnection);
        });

    // this runs after unsubscribeFromRegistry("device/"), so each operator's
    // own subscription has to be re-registered here
    if (auto *deviceAdapter = qobject_cast<DeviceAdapter *>(obj)) {
      resubscribe_operator_adapters(*this, deviceAdapter->operatorAdapters());
    }
  }

  int selectedDeviceIndex = _workspace.config.selectedDeviceIndex.value();
  if (selectedDeviceIndex >= _deviceAdapters.size() || selectedDeviceIndex < 0)
    selectedDeviceIndex = 0;

  const bool group_still_selected =
      _selectedNodeKind == QStringLiteral("group") &&
      find_device_group_index_by_id(_workspace.config,
                                    _selectedNodeId.toStdString()) >= 0;

  pc::logger()->trace(
      "syncDeviceAdapters: selection — index={}, group_still_selected={}, "
      "selectedNodeKind='{}', selectedNodeId='{}'",
      selectedDeviceIndex, group_still_selected,
      _selectedNodeKind.toStdString(), _selectedNodeId.toStdString());

  if (group_still_selected) {
    if (_selectedDeviceIndex != selectedDeviceIndex) {
      _selectedDeviceIndex = selectedDeviceIndex;
      _workspace.config.selectedDeviceIndex = selectedDeviceIndex;
      emit selectedDeviceIndexChanged();
    }
  } else {
    setSelectedDeviceIndex(selectedDeviceIndex);
  }

  refreshSelectedGroupHasSequence();

  emit deviceAdaptersChanged();
  emit deviceVariantNamesChanged();
  emit addDeviceMenuEntriesChanged();
  emit deviceTreeRowsChanged();
  emit selectedOperatorFrameSourceChanged();

  pc::logger()->trace("syncDeviceAdapters: done, workspace device count={}",
                      _workspace.devices.size());
}

void WorkspaceModel::triggerDeviceDiscovery() {
  for (const auto &plugin_name : _workspace.loaded_device_plugin_names) {
    auto it = _workspace.discovery_plugins.find(plugin_name);
    if (it == _workspace.discovery_plugins.end() || !it->second) continue;
    auto &discovery_device = it->second;
    if (!discovery_device->has_discovery_change_callback()) {
      discovery_device->add_discovery_change_callback([this] {
        QMetaObject::invokeMethod(
            this, [this] { emit addDeviceMenuEntriesChanged(); },
            Qt::QueuedConnection);
      });
    }
    discovery_device->refresh_discovery();
  }
}

bool WorkspaceModel::isDescendantOf(
    const std::string &node_id, const std::string &maybe_ancestor_id) const {
  std::string current_id = node_id;

  if (find_device_group_index_by_id(_workspace.config, node_id) < 0) {
    const int device_index =
        find_device_index_by_id(_workspace.config, node_id);
    if (device_index < 0) return false;
    current_id = std::visit(
        [](const auto &device_config) {
          return device_config.parent_id.value();
        },
        _workspace.config.devices[size_t(device_index)]);
  }

  while (!current_id.empty()) {
    if (current_id == maybe_ancestor_id) return true;
    const int group_index =
        find_device_group_index_by_id(_workspace.config, current_id);
    if (group_index < 0) break;
    current_id =
        _workspace.config.device_groups[size_t(group_index)].parent_id.value();
  }
  return false;
}

void WorkspaceModel::createDeviceGroup(const QString &parent_id) {
  auto new_config = _workspace.config;

  // reject a parent that doesn't exist (empty string is valid, root level)
  const std::string parent = parent_id.toStdString();
  if (!parent.empty() &&
      find_device_group_index_by_id(new_config, parent) < 0) {
    pc::logger()->warn("createDeviceGroup: parent group not found '{}'",
                       parent);
    return;
  }

  pc::devices::DeviceGroupConfiguration group_config{.id = pc::uuid::word()};

  const auto existing_device_groups =
      std::ranges::transform_view(new_config.device_groups, [](auto &group) {
        return group.label.value();
      });

  const auto label =
      util::next_available_label("group", existing_device_groups);
  group_config.label.set(std::move(label));

  group_config.parent_id.set(parent);
  new_config.device_groups.push_back(std::move(group_config));

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

bool WorkspaceModel::deleteDeviceGroup(const QString &group_id) {
  auto new_config = _workspace.config;
  const std::string target_group_id = group_id.toStdString();

  const int group_index =
      find_device_group_index_by_id(new_config, target_group_id);
  if (group_index < 0) {
    pc::logger()->warn("deleteDeviceGroup: group id not found '{}'",
                       target_group_id);
    return false;
  }

  // children (groups and devices) are reparented to the deleted group's parent
  const std::string surviving_parent_id =
      new_config.device_groups[size_t(group_index)].parent_id.value();

  // update every child's path
  const auto group_prefix = node_path_prefix(new_config, target_group_id);
  std::vector<std::pair<std::string, std::string>> child_prefixes;
  for (const auto &group_config : new_config.device_groups) {
    if (group_config.parent_id.value() == target_group_id)
      child_prefixes.emplace_back(
          group_config.id, node_path_prefix(new_config, group_config.id));
  }
  for (const auto &device_variant : new_config.devices) {
    std::visit(
        [&](const auto &device_config) {
          if (device_config.parent_id.value() == target_group_id)
            child_prefixes.emplace_back(
                device_config.id,
                node_path_prefix(new_config, device_config.id));
        },
        device_variant);
  }

  for (auto &group_config : new_config.device_groups) {
    if (group_config.parent_id.value() == target_group_id)
      group_config.parent_id.set(surviving_parent_id);
  }
  for (auto &device_variant : new_config.devices) {
    std::visit(
        [&](auto &device_config) {
          if (device_config.parent_id.value() == target_group_id)
            device_config.parent_id.set(surviving_parent_id);
        },
        device_variant);
  }

  new_config.device_groups.erase(new_config.device_groups.begin() +
                                 group_index);

  for (const auto &[child_id, previous_prefix] : child_prefixes)
    reroot_node_paths(new_config, previous_prefix, child_id);
  erase_node_paths(new_config, group_prefix);
  erase_session_node_state(new_config, target_group_id);

  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);

  return true;
}

void WorkspaceModel::setDeviceGroupCollapsed(const QString &group_id,
                                             bool collapsed) {
  auto new_config = _workspace.config;
  const int group_index =
      find_device_group_index_by_id(new_config, group_id.toStdString());
  if (group_index < 0) return;
  new_config.device_groups[size_t(group_index)].collapsed.set(collapsed);
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

void WorkspaceModel::setDeviceGroupLabel(const QString &group_id,
                                         const QString &label) {
  const auto id = group_id.toStdString();
  auto new_config = _workspace.config;
  const int group_index = find_device_group_index_by_id(new_config, id);
  if (group_index < 0) return;
  const auto previous_prefix = node_path_prefix(new_config, id);
  new_config.device_groups[size_t(group_index)].label.set(label.toStdString());
  reroot_node_paths(new_config, previous_prefix, id);
  applyWorkspaceConfigAndRebuild(std::move(new_config), RebuildScope::Devices);
}

QVariantList WorkspaceModel::deviceTreeRows() const {
  QVariantList rows;

  // the switches in the list belong to whichever session is selected
  const int selected_session_index = find_session_index_by_id(
      _workspace.config, _selectedSessionId.toStdString());
  const pc::SessionConfiguration *selected_session =
      selected_session_index >= 0
          ? &_workspace.config
                 .sessions[static_cast<size_t>(selected_session_index)]
          : (_workspace.config.sessions.empty()
                 ? nullptr
                 : &_workspace.config.sessions.front());

  const auto switched_on_here = [&](const std::string &node_id) {
    if (!selected_session) return true;
    return !selected_session->disabled_devices.value().contains(node_id);
  };
  const auto switched_on_through_parents = [&](const std::string &node_id) {
    if (!selected_session) return true;
    return pc::devices::effective_session_enabled(_workspace.config,
                                                  *selected_session, node_id);
  };

  struct Entry {
    bool is_group;
    int order;
    int index;
  };

  std::function<void(const std::string &, int)> walk =
      [&](const std::string &parent_id, int depth) {
        std::vector<Entry> entries;

        for (int group_index = 0;
             group_index < int(_workspace.config.device_groups.size());
             group_index++) {
          const auto &group_config =
              _workspace.config.device_groups[size_t(group_index)];
          if (group_config.parent_id.value() == parent_id)
            entries.push_back({true, group_config.order.value(), group_index});
        }
        for (int i = 0; i < int(_workspace.config.devices.size()); ++i) {
          const auto &device_variant = _workspace.config.devices[size_t(i)];
          const std::string device_parent = std::visit(
              [](const auto &device_config) {
                return device_config.parent_id.value();
              },
              device_variant);
          if (device_parent != parent_id) continue;
          const int device_order = std::visit(
              [](const auto &device_config) {
                return device_config.order.value();
              },
              device_variant);
          entries.push_back({false, device_order, i});
        }

        std::stable_sort(
            entries.begin(), entries.end(),
            [](const Entry &a, const Entry &b) { return a.order < b.order; });

        for (const auto &entry : entries) {
          if (entry.is_group) {
            const auto &group_config =
                _workspace.config.device_groups[size_t(entry.index)];
            QVariantMap group_row;
            group_row["kind"] = "group";
            group_row["id"] = QString::fromStdString(group_config.id);
            group_row["parentId"] = QString::fromStdString(parent_id);
            group_row["label"] =
                QString::fromStdString(group_config.label.value());
            group_row["depth"] = depth;
            group_row["collapsed"] = group_config.collapsed.value();
            group_row["sessionEnabled"] = switched_on_here(group_config.id);
            group_row["effectiveSessionEnabled"] =
                switched_on_through_parents(group_config.id);
            rows.push_back(std::move(group_row));
            if (!group_config.collapsed.value())
              walk(group_config.id, depth + 1);
          } else {
            const auto &device_variant =
                _workspace.config.devices[size_t(entry.index)];
            const std::string device_id = std::visit(
                [](const auto &device_config) { return device_config.id; },
                device_variant);
            const std::string device_label = std::visit(
                [](const auto &device_config) {
                  return device_config.label.value();
                },
                device_variant);
            QVariantMap device_row;
            device_row["kind"] = "device";
            device_row["id"] = QString::fromStdString(device_id);
            device_row["parentId"] = QString::fromStdString(parent_id);
            device_row["label"] = QString::fromStdString(device_label);
            device_row["deviceIndex"] = entry.index;
            device_row["depth"] = depth;
            device_row["sessionEnabled"] = switched_on_here(device_id);
            device_row["effectiveSessionEnabled"] =
                switched_on_through_parents(device_id);
            rows.push_back(std::move(device_row));
          }
        }
      };

  walk("", 0);
  return rows;
}

void WorkspaceModel::moveDeviceNode(const QString &node_id,
                                    const QString &new_parent_id,
                                    const QString &before_node_id) {
  const std::string id = node_id.toStdString();
  const std::string new_parent = new_parent_id.toStdString();
  const std::string before = before_node_id.toStdString();

  if (id.empty() || id == new_parent) return;

  // accessors that work uniformly across groups and the device variant
  const auto node_parent =
      [&](const std::string &nid) -> std::optional<std::string> {
    if (const int group_index =
            find_device_group_index_by_id(_workspace.config, nid);
        group_index >= 0)
      return _workspace.config.device_groups[size_t(group_index)]
          .parent_id.value();
    for (const auto &device_variant : _workspace.config.devices) {
      std::optional<std::string> found;
      std::visit(
          [&](const auto &device_config) {
            if (device_config.id == nid)
              found = device_config.parent_id.value();
          },
          device_variant);
      if (found.has_value()) return found;
    }
    return std::nullopt;
  };
  const auto set_parent = [&](const std::string &nid,
                              const std::string &parent) {
    if (const int group_index =
            find_device_group_index_by_id(_workspace.config, nid);
        group_index >= 0) {
      _workspace.config.device_groups[size_t(group_index)].parent_id = parent;
      return;
    }
    for (auto &device_variant : _workspace.config.devices)
      std::visit(
          [&](auto &device_config) {
            if (device_config.id == nid) device_config.parent_id = parent;
          },
          device_variant);
  };
  const auto set_order = [&](const std::string &nid, int order) {
    if (const int group_index =
            find_device_group_index_by_id(_workspace.config, nid);
        group_index >= 0) {
      _workspace.config.device_groups[size_t(group_index)].order = order;
      return;
    }
    for (auto &device_variant : _workspace.config.devices)
      std::visit(
          [&](auto &device_config) {
            if (device_config.id == nid) device_config.order = order;
          },
          device_variant);
  };

  // node must exist
  if (!node_parent(id).has_value()) return;

  const auto previous_prefix = node_path_prefix(_workspace.config, id);

  // prevent moving a group into its own subtree (walk up from the new parent)
  for (std::string p = new_parent; !p.empty();) {
    if (p == id) return;
    const auto pp = node_parent(p);
    if (!pp.has_value()) break;
    p = *pp;
  }

  // reparent first so sibling gathering reflects the destination
  set_parent(id, new_parent);

  // gather destination siblings (both kinds), excluding the moved node,
  // in their current visual order
  struct Sibling {
    std::string id;
    int order;
  };
  std::vector<Sibling> siblings;
  for (const auto &group_config : _workspace.config.device_groups) {
    if (group_config.id == id) continue;
    if (group_config.parent_id.value() == new_parent)
      siblings.push_back({group_config.id, group_config.order.value()});
  }
  for (const auto &device_variant : _workspace.config.devices) {
    std::string did;
    std::string dparent;
    int dorder = 0;
    std::visit(
        [&](const auto &device_config) {
          did = device_config.id;
          dparent = device_config.parent_id.value();
          dorder = device_config.order.value();
        },
        device_variant);
    if (did == id) continue;
    if (dparent == new_parent) siblings.push_back({did, dorder});
  }
  std::stable_sort(
      siblings.begin(), siblings.end(),
      [](const Sibling &a, const Sibling &b) { return a.order < b.order; });

  // resolve insertion position (append when before is empty or not found)
  size_t insert_at = siblings.size();
  if (!before.empty()) {
    for (size_t i = 0; i < siblings.size(); ++i) {
      if (siblings[i].id == before) {
        insert_at = i;
        break;
      }
    }
  }

  // build the resulting order and reassign contiguous indices to all siblings
  std::vector<std::string> ordered;
  ordered.reserve(siblings.size() + 1);
  for (size_t i = 0; i < siblings.size(); ++i) {
    if (i == insert_at) ordered.push_back(id);
    ordered.push_back(siblings[i].id);
  }
  if (insert_at >= siblings.size()) ordered.push_back(id);

  for (int i = 0; i < int(ordered.size()); ++i)
    set_order(ordered[size_t(i)], i);

  reroot_node_paths(_workspace.config, previous_prefix, id);

  _workspace.rebuild_config_registry();
  syncDeviceAdapters();
  emit publishPathsChanged();
  emit pushPathsChanged();
  emit renderPathsChanged();
  _streamChannelModel->refresh();
}

} // namespace pc::ui