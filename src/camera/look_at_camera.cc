#include "look_at_camera.h"
#include "camera_frame.h"
#include "look_at_camera_config.h"

#include <cmath>
#include <logger/logger.h>
#include <numbers>
#include <plugins/backend/backend_plugin.h>
#include <pointcaster/core_types.h>
#include <pointcaster/point_cloud.h>

namespace pc::camera {

void LookAtCamera::update_config(const LookAtCameraConfiguration &config) {

  auto &p = _projection_args;

  p.pixel_width = config.resolution_x;
  p.pixel_height = config.resolution_y;

  const float fov_y_radians =
      config.vertical_fov * std::numbers::pi_v<float> / 180.0f;
  const float cot_half_fov_y = 1.0f / std::tan(fov_y_radians * 0.5f);
  p.fy = (static_cast<float>(p.pixel_height) * 0.5f) * cot_half_fov_y;
  p.fx = p.fy;
  p.cx = static_cast<float>(p.pixel_width) * 0.5f;
  p.cy = static_cast<float>(p.pixel_height) * 0.5f;

  // for mm to m conversion
  constexpr float pos_scale = 1000.0f;

  const auto raw_target = config.look_at_position;
  const auto raw_pos = config.position;
  const pc::float3 target{raw_target.x * pos_scale, raw_target.y * pos_scale,
                          raw_target.z * pos_scale};
  const pc::float3 pos{raw_pos.x * pos_scale, raw_pos.y * pos_scale,
                       raw_pos.z * pos_scale};

  constexpr pc::float3 world_up{0.0f, 1.0f, 0.0f};

  const float fwx = target.x - pos.x, fwy = target.y - pos.y,
              fwz = target.z - pos.z;
  const float fwd_len = std::sqrt(fwx * fwx + fwy * fwy + fwz * fwz);
  const float fx = fwx / fwd_len;
  const float fy = fwy / fwd_len;
  const float fz = fwz / fwd_len;

  float rx = fy * world_up.z - fz * world_up.y;
  float ry = fz * world_up.x - fx * world_up.z;
  float rz = fx * world_up.y - fy * world_up.x;
  const float r_len = std::sqrt(rx * rx + ry * ry + rz * rz);
  rx /= r_len;
  ry /= r_len;
  rz /= r_len;

  const float dx = fy * rz - fz * ry;
  const float dy = fz * rx - fx * rz;
  const float dz = fx * ry - fy * rx;

  p.camera_extrinsic.values = {
      rx, ry, rz, -(rx * pos.x + ry * pos.y + rz * pos.z),
      //
      dx, dy, dz, -(dx * pos.x + dy * pos.y + dz * pos.z),
      //
      fx, fy, fz, -(fx * pos.x + fy * pos.y + fz * pos.z),
      //
      0, 0, 0, 1};

  _flood_fill_passes = config.color_fill_passes;

} // namespace pc::camera

void LookAtCamera::project(const PointCloud &cloud,
                           backend::BackendPlugin *backend) {

  if (!backend) {
    pc::logger()->error(
        "Uninitialised backend passed into LookAtCamera::project");
    return;
  }

  if (cloud.empty()) return;

  const auto &p = _projection_args;

  const auto pixel_count = p.pixel_width * p.pixel_height;

  auto result =
      std::make_shared<CameraFrameData>(p.pixel_width, p.pixel_height);

  backend->project_frame(cloud, *result, _projection_args);

  // TODO probs shouldn't be a member func, should take colour and depth buffer
  // refs instead and be a function on the backend right?

  result->flood_fill(_flood_fill_passes);

  _result = result;
}

} // namespace pc::camera