#pragma once

#include <pointcaster/core_types.h>
#include <string>

namespace pc {

struct LookAtCameraConfiguration {
  std::string id; // @hidden

  bool locked = false; // @hidden

  bool orthographic = false;

  float3 position = float3(0, 0, -2.5);      // @minmax(-10, 10)
  float3 look_at_position = float3(0, 0, 0); // @minmax(-10, 10)

  float vertical_fov = 60.0f; // @minmax(5, 355)

  int resolution_x = 400; // @minmax(64, 4096)
  int resolution_y = 300; // @minmax(64, 4096)

  int color_fill_passes = 0; // @minmax(0, 20)
};

} // namespace pc