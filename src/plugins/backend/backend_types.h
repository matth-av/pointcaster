#pragma once

#include <functional>
#include <limits>
#include <pointcaster/core_types.h>
#include <span>
#include <tuple>
#include <vector>

namespace pc {

enum class BackendType { CPU, CUDA };

namespace backend {

struct CameraIntrinsics {
  float fx, fy, cx, cy;
  float inv_fx, inv_fy;
  size_t frame_width;
};

struct Extrinsics {
  float r[9]; // row-major 3x3
  float t[3];
};

inline constexpr float invalid_xy_scale_component =
    std::numeric_limits<float>::max();

// used to undistort camera sensors that can't use pinhole projection
// (e.g. kinect which ships its own xy_table for manual undistortion we can
// paralellise outside its own api)
struct XyTable {
  std::vector<float2> xy_scales;
};

using PointType = std::tuple<position, color>;

struct BoundsFilterOptions {
  bool invert = false;
  bool analyse_only = false;
};

struct BoundsFilterResult {
  size_t point_count = 0;
  size_t input_count = 0;
  position_bounds bounds{};
};

// return an internal point type given pixel data from a depth frame
using PointTransformer =
    std::function<PointType(const int, const uint16_t, const color_rgb &)>;

} // namespace backend

} // namespace pc
