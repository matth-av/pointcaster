#pragma once

#include "backend_types.h"

#include <cmath>
#include <config/color_transform_config.h>
#include <config/transform_config.h>
#include <limits>
#include <optional>
#include <pointcaster/core_types.h>

#ifdef __CUDACC__
#define PC_DEVICE_FUNC __host__ __device__
#include <thrust/tuple.h>
#else
#define PC_DEVICE_FUNC
#endif

namespace pc::backend::filter {

static constexpr position invalid_position_value{-32768, -32768, 32767};

struct TransformFilterParameters {

  float position_x, position_y, position_z;
  float rotation_matrix[9];
  float scale_x, scale_y, scale_z;
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
  int sample;

  struct ColorParameters {
    float gain;
  } color;

  static inline TransformFilterParameters
  from_config(const pc::TransformConfiguration &transform_config,
              const pc::ColorTransformConfiguration &color_config) {

    const auto &position = transform_config.position;
    const auto &rotation = transform_config.rotation;
    const auto &scale = transform_config.scale;

    auto min_x = static_cast<float>(std::numeric_limits<int16_t>::min());
    auto min_y = static_cast<float>(std::numeric_limits<int16_t>::min());
    auto min_z = static_cast<float>(std::numeric_limits<int16_t>::min());
    auto max_x = static_cast<float>(std::numeric_limits<int16_t>::max());
    auto max_y = static_cast<float>(std::numeric_limits<int16_t>::max());
    auto max_z = static_cast<float>(std::numeric_limits<int16_t>::max());

    const auto &crop = transform_config.bounds;
    if (crop.active) {
      const auto &min_bound = crop.value.min;
      min_x = static_cast<float>(min_bound.x);
      min_y = static_cast<float>(min_bound.y);
      min_z = static_cast<float>(min_bound.z);
      const auto &max_bound = crop.value.max;
      max_x = static_cast<float>(max_bound.x);
      max_y = static_cast<float>(max_bound.y);
      max_z = static_cast<float>(max_bound.z);
    }

    constexpr float deg_2_rad = 3.14159265358979f / 180.0f;
    const float cx = std::cos(rotation.x * deg_2_rad);
    const float sxr = std::sin(rotation.x * deg_2_rad);
    const float cy = std::cos(rotation.y * deg_2_rad);
    const float sy = std::sin(rotation.y * deg_2_rad);
    const float cz = std::cos(rotation.z * deg_2_rad);
    const float szr = std::sin(rotation.z * deg_2_rad);

    return {.position_x = position.x * 1000.f,
            .position_y = position.y * 1000.f,
            .position_z = position.z * 1000.f,
            .rotation_matrix = {cy * cz + sy * sxr * szr,
                                sy * sxr * cz - cy * szr, sy * cx, cx * szr,
                                cx * cz, -sxr, cy * sxr * szr - sy * cz,
                                sy * szr + cy * sxr * cz, cy * cx},
            .scale_x = scale.x,
            .scale_y = scale.y,
            .scale_z = scale.z,
            .min_x = min_x,
            .min_y = min_y,
            .min_z = min_z,
            .max_x = max_x,
            .max_y = max_y,
            .max_z = max_z,
            .sample = transform_config.sample,
            .color = {.gain = color_config.gain}};
  }
};

PC_DEVICE_FUNC inline position
transform(position pos, const TransformFilterParameters &param) {
  auto [x, y, z, _p] = pos;

  x *= param.scale_x;
  y *= param.scale_y;
  z *= param.scale_z;

  const auto &r = param.rotation_matrix;
  const float rx = r[0] * x + r[1] * y + r[2] * z;
  const float ry = r[3] * x + r[4] * y + r[5] * z;
  const float rz = r[6] * x + r[7] * y + r[8] * z;

  return {static_cast<int16_t>(rx + param.position_x),
          static_cast<int16_t>(ry + param.position_y),
          static_cast<int16_t>(rz + param.position_z)};
}

PC_DEVICE_FUNC inline color
color_transform(color col, const TransformFilterParameters &param) {
  float r = col.r * param.color.gain;
  float g = col.g * param.color.gain;
  float b = col.b * param.color.gain;

  return {static_cast<unsigned char>(r), static_cast<unsigned char>(g),
          static_cast<unsigned char>(b)};
}

PC_DEVICE_FUNC inline bool in_bounds(position pos,
                                     const TransformFilterParameters &param) {
  return (pos.x >= param.min_x && pos.x <= param.max_x) &&
         (pos.y >= param.min_y && pos.y <= param.max_y) &&
         (pos.z >= param.min_z && pos.z <= param.max_z);
}

PC_DEVICE_FUNC inline bool sample(uint32_t idx,
                                  const TransformFilterParameters &p) {
  return (idx % p.sample) == 0;
}

PC_DEVICE_FUNC inline bool is_valid(position p) {
  return p.x != invalid_position_value.x || p.y != invalid_position_value.y ||
         p.z != invalid_position_value.z;
}

PC_DEVICE_FUNC inline bool in_bounds(const position &p,
                                     const position_bounds &bounds) {
  return p.x >= bounds.min.x && p.x <= bounds.max.x && p.y >= bounds.min.y &&
         p.y <= bounds.max.y && p.z >= bounds.min.z && p.z <= bounds.max.z;
}

PC_DEVICE_FUNC inline position_bounds empty_bounds() {
  constexpr int16_t lowest = -32768;
  constexpr int16_t highest = 32767;
  return {{highest, highest, highest}, {lowest, lowest, lowest}};
}

PC_DEVICE_FUNC inline position_bounds as_bounds(const position &p) {
  return {p, p};
}

PC_DEVICE_FUNC inline position_bounds merge_bounds(const position_bounds &a,
                                                   const position_bounds &b) {
  return {{a.min.x < b.min.x ? a.min.x : b.min.x,
           a.min.y < b.min.y ? a.min.y : b.min.y,
           a.min.z < b.min.z ? a.min.z : b.min.z},
          {a.max.x < b.max.x ? b.max.x : a.max.x,
           a.max.y < b.max.y ? b.max.y : a.max.y,
           a.max.z < b.max.z ? b.max.z : a.max.z}};
}

// how many points passed a test and the box they fill, so a single reduction
// answers both instead of one pass counting and another measuring
struct bounds_measurement {
  size_t point_count;
  position_bounds bounds;
};

PC_DEVICE_FUNC inline bounds_measurement empty_measurement() {
  return {0, empty_bounds()};
}

PC_DEVICE_FUNC inline bounds_measurement as_measurement(const position &p) {
  return {1, as_bounds(p)};
}

PC_DEVICE_FUNC inline bounds_measurement
merge_measurements(const bounds_measurement &a, const bounds_measurement &b) {
  return {a.point_count + b.point_count, merge_bounds(a.bounds, b.bounds)};
}

} // namespace pc::backend::filter