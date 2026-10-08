#pragma once

#include <memory>
#include <pointcaster/point_cloud.h>
#include <string>
#include <vector>

namespace pc {
class Workspace;
}

// TODO i feel like all this doesn't belong in this namespace or this filename

namespace pc::networking {

struct PointStream {
  std::string address;
  std::shared_ptr<PointCloud> cloud;
  std::vector<std::shared_ptr<PointCloud>> child_clouds;
};

std::vector<PointStream> collect_point_streams(Workspace &workspace);

} // namespace pc::networking
