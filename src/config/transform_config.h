#pragma once

#include <config/core_types_reflection.h>
#include <plugins/backend/backend_types.h>
#include <pointcaster/config.h>
#include <pointcaster/core_types.h>

namespace pc {

struct TransformConfiguration {
  float3 position = float3(0, 0, 0); // @minmax(-10, 10)
  float3 rotation = float3(0, 0, 0); // @minmax(-360, 360)
  float3 scale = float3(1, 1, 1);    // @minmax(-1, 2.5)

  Toggleable<float> point_scale =
      Toggleable<float>{false, 1.0f}; // @minmax(0, 10)

  Toggleable<position_bounds> bounds = pc::default_crop_bounds;

  int sample = 1; // @minmax(1, 64)

  BackendType backend = BackendType::CPU;
};

POINTCASTER_CONFIG_EXPORT float4x4 to_float4x4(const TransformConfiguration &t);

} // namespace pc