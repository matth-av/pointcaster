#pragma once

#include <functional>
#include <optional>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <plugins/devices/device_group_config.h>
#include <plugins/devices/device_variants.h>
#include <point_streamer/point_streamer_config.h>
#include <publishers/publishers_config.h>
#include <receivers/osc/osc_receiver_config.h>
#include <rfl/DefaultVal.hpp>
#include <session/session_config.h>
#include <set>
#include <string>

namespace pc {

// WorkspaceConfiguration is the config that gets de/serialized and holds all
// simulation state for the running application.
struct WorkspaceConfiguration {
  std::string id;
  rfl::DefaultVal<std::string> selectedSessionId;
  rfl::DefaultVal<int> selectedDeviceIndex = 0;
  std::vector<SessionConfiguration> sessions{};
  std::vector<devices::DeviceConfigurationVariant> devices{};
  std::vector<devices::DeviceGroupConfiguration> device_groups;
  rfl::DefaultVal<publishers::PublishersConfiguration> publishers;
  rfl::DefaultVal<networking::PointStreamerConfiguration> point_streamer;
  rfl::DefaultVal<receivers::OscReceiverConfiguration> osc_receiver;

  // config field paths that should be published
  rfl::DefaultVal<std::set<std::string>> publish_paths;
  rfl::DefaultVal<std::set<std::string>> push_paths;
  // output stream paths that should be drawn in the 3d scene, kept apart from
  // publishing so a stream can be looked at without leaving the machine
  rfl::DefaultVal<std::set<std::string>> render_paths;
};

bool load_workspace_from_file(WorkspaceConfiguration &config,
                              const std::string &file_path,
                              bool load_malformed);

void save_workspace_to_file(const WorkspaceConfiguration &config,
                            const std::string &file_path);

std::optional<std::reference_wrapper<SessionConfiguration>>
session_config_from_workspace(const WorkspaceConfiguration &config,
                              std::string_view session_id);

} // namespace pc