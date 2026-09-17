#pragma once

#include "ply_device_config.h"

#include <memory>
#include <optional>
#include <pointcaster/point_cloud.h>
#include <span>
#include <string>
#include <vector>

namespace pc::devices::ply {

using PositionUnits = PlyDeviceConfiguration::PositionUnits;

struct AttributeInfo {
  std::string name;
  std::string type_name;
};

struct FileInfo {
  size_t vertex_count = 0;
  bool floating_point_positions = false;
  std::vector<AttributeInfo> attributes;
};

// reads only the ply header
std::optional<FileInfo> scan_file_info(const std::string &path);

// only the properties given a target in attributes are read into the cloud

std::shared_ptr<PointCloud>
read_point_cloud(const std::string &path, PositionUnits units,
                 std::span<const AttributeConfiguration> attributes = {});

} // namespace pc::devices::ply
