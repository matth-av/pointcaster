#include "device_tree.h"

#include <config/transform_config.h>
#include <core/util/geometry_utils.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <workspace/workspace_config.h>

namespace pc::devices {

bool effective_session_enabled(const pc::WorkspaceConfiguration &config,
                               const pc::SessionConfiguration &session_config,
                               const std::string &node_id) {
  const auto &disabled_nodes = session_config.disabled_devices;
  if (disabled_nodes.empty()) return true;
  if (disabled_nodes.contains(node_id)) return false;

  std::string parent_id;
  bool found_node = false;

  if (const int group_index = group_index_by_id(config, node_id);
      group_index >= 0) {
    parent_id =
        config.device_groups[static_cast<size_t>(group_index)].parent_id;
    found_node = true;
  } else {
    for (const auto &device_variant : config.devices) {
      std::visit(
          [&](const auto &device_config) {
            if (device_config.id == node_id) {
              parent_id = device_config.parent_id;
              found_node = true;
            }
          },
          device_variant);
      if (found_node) break;
    }
  }

  if (!found_node) return true;

  for (std::string ancestor_id = parent_id; !ancestor_id.empty();) {
    if (disabled_nodes.contains(ancestor_id)) return false;
    const int group_index = group_index_by_id(config, ancestor_id);
    if (group_index < 0) break;
    ancestor_id =
        config.device_groups[static_cast<size_t>(group_index)].parent_id;
  }
  return true;
}

bool in_any_session(const pc::WorkspaceConfiguration &config,
                    const std::string &node_id) {
  for (const auto &session_config : config.sessions) {
    if (effective_session_enabled(config, session_config, node_id)) return true;
  }
  return false;
}

std::vector<std::string>
all_node_ids(const pc::WorkspaceConfiguration &config) {
  std::vector<std::string> node_ids;
  node_ids.reserve(config.devices.size() + config.device_groups.size());
  for (const auto &device_variant : config.devices) {
    std::visit(
        [&](const auto &device_config) {
          node_ids.push_back(device_config.id);
        },
        device_variant);
  }
  for (const auto &group_config : config.device_groups)
    node_ids.push_back(group_config.id);
  return node_ids;
}

pc::float4x4 effective_world_transform(const pc::WorkspaceConfiguration &config,
                                       const std::string &node_id) {
  std::string parent;
  bool found = false;

  if (const int group_index = group_index_by_id(config, node_id);
      group_index >= 0) {
    parent = config.device_groups[size_t(group_index)].parent_id;
    found = true;
  } else {
    for (const auto &device_variant : config.devices) {
      std::visit(
          [&](const auto &device_config) {
            if (device_config.id == node_id) {
              parent = device_config.parent_id;
              found = true;
            }
          },
          device_variant);
      if (found) break;
    }
  }
  if (!found) return {};

  pc::float4x4 world; // identity
  for (std::string p = parent; !p.empty();) {
    const int group_index = group_index_by_id(config, p);
    if (group_index < 0) break;
    world = pc::multiply(
        pc::to_float4x4(config.device_groups[size_t(group_index)].transform),
        world);
    p = config.device_groups[size_t(group_index)].parent_id;
  }
  return world;
}

int group_index_by_id(const pc::WorkspaceConfiguration &config,
                      const std::string &id) {
  for (int i = 0; i < int(config.device_groups.size()); ++i)
    if (config.device_groups[size_t(i)].id == id) return i;
  return -1;
}

namespace {
std::string label_or_id(const std::string &label, const std::string &id) {
  return !label.empty() ? label : id;
}
} // namespace

std::string device_address(const pc::WorkspaceConfiguration &config,
                           const std::string &device_id) {
  std::string own_label;
  std::string parent;
  bool found = false;

  if (const int group_index = group_index_by_id(config, device_id);
      group_index >= 0) {
    const auto &group_config =
        config.device_groups[static_cast<size_t>(group_index)];
    own_label = label_or_id(group_config.label, group_config.id);
    parent = group_config.parent_id;
    found = true;
  } else {
    for (const auto &device_variant : config.devices) {
      std::visit(
          [&](const auto &device_config) {
            if (device_config.id == device_id) {
              own_label = label_or_id(device_config.label, device_config.id);
              parent = device_config.parent_id;
              found = true;
            }
          },
          device_variant);
      if (found) break;
    }
  }

  if (!found) return device_id;

  std::vector<std::string> ancestor_labels;
  for (std::string ancestor_id = parent; !ancestor_id.empty();) {
    const int group_index = group_index_by_id(config, ancestor_id);
    if (group_index < 0) break;
    const auto &group_config =
        config.device_groups[static_cast<size_t>(group_index)];
    ancestor_labels.push_back(label_or_id(group_config.label, group_config.id));
    ancestor_id = group_config.parent_id;
  }

  std::string address;
  for (auto label = ancestor_labels.rbegin(); label != ancestor_labels.rend();
       ++label) {
    address += *label;
    address += '/';
  }
  address += own_label;
  return address;
}

std::vector<std::string>
device_ids_in_group(const pc::WorkspaceConfiguration &config,
                    const std::string &group_id) {
  std::vector<std::string> result;
  for (const auto &device_variant : config.devices) {
    std::string device_id;
    std::string parent_id;
    std::visit(
        [&](const auto &device_config) {
          using T = std::decay_t<decltype(device_config)>;
          if constexpr (!std::same_as<T, DeviceGroupConfiguration>) {
            device_id = device_config.id;
            parent_id = device_config.parent_id;
          }
        },
        device_variant);
    if (device_id.empty()) continue;
    std::string current_id = parent_id;
    while (!current_id.empty()) {
      if (current_id == group_id) {
        result.push_back(device_id);
        break;
      }
      const int group_index = group_index_by_id(config, current_id);
      if (group_index < 0) break;
      current_id = config.device_groups[size_t(group_index)].parent_id;
    }
  }
  return result;
}

} // namespace pc::devices