#include "stream_channels.h"

#include <plugins/devices/device_tree.h>
#include <pointcaster/point_cloud.h>
#include <session/session.h>
#include <workspace/workspace.h>

namespace pc::networking {

// return all possible PointCloud stream channels that are available for
// use through the workspace
std::vector<PointStream> collect_point_streams(Workspace &workspace) {
  std::vector<PointStream> sources;
  // TODO maybe this lock situation needs to be re thought through. we access
  // workspace.config here without the lock, lock below, then access it later
  // again for groups without the lock.
  const auto sources_count = workspace.config.sessions.size() +
                             workspace.devices.size() +
                             workspace.config.device_groups.size();
  sources.reserve(sources_count);

  {
    std::scoped_lock lock(workspace.sessions_access);
    for (const auto &session_config : workspace.config.sessions) {
      const auto &label = session_config.label;
      std::string address = !label.empty() ? label : session_config.id;

      std::shared_ptr<PointCloud> cloud;
      if (auto it = workspace.sessions.find(session_config.id);
          it != workspace.sessions.end() && it->second) {
        cloud = it->second->point_cloud();
      }
      sources.push_back({std::move(address), std::move(cloud)});
    }
  }

  for (auto &device : workspace.devices) {
    if (!device) continue;
    std::string id;
    std::visit([&](const auto &cfg) { id = cfg.id; }, device->config());
    auto cloud = devices::in_any_session(workspace.config, id)
                     ? device->point_cloud()
                     : nullptr;
    sources.push_back(
        {devices::device_address(workspace.config, id), std::move(cloud)});
  }

  for (const auto &group : workspace.config.device_groups) {
    const auto child_ids =
        devices::device_ids_in_group(workspace.config, group.id);
    if (child_ids.empty()) continue;

    std::shared_ptr<PointCloud> merged;
    for (const auto &child_id : child_ids) {
      for (auto &device : workspace.devices) {
        if (!device) continue;
        std::string device_id;
        std::visit([&](const auto &cfg) { device_id = cfg.id; },
                   device->config());
        if (device_id != child_id) continue;
        const auto cloud = device->point_cloud();
        if (cloud && !cloud->empty()) {
          if (!merged)
            merged = std::make_shared<PointCloud>(*cloud);
          else
            *merged += *cloud;
        }
        break;
      }
    }

    sources.push_back({devices::device_address(workspace.config, group.id),
                       std::move(merged)});
  }

  return sources;
}

} // namespace pc::networking
