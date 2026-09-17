#pragma once

#include <camera/look_at_camera_config.h>
#include <config/color_transform_config.h>
#include <plugins/backend/backend_types.h>
#include <rfl/Literal.hpp>
#include <string>

namespace pc::operators {

class FringeRemovalOperator;

struct FringeRemovalConfiguration {
  std::string id;     // @hidden
  std::string label;  // @hidden
  bool active = true; // @hidden

  LookAtCameraConfiguration camera;

  // edge detection
  float depth_threshold = 0.02f; // @minmax(0.005, 0.2)
  int max_search_neighbors = 50; // @minmax(5, 200)
  float canny_low = 40.0f;       // @minmax(1, 255)
  float canny_high = 100.0f;     // @minmax(1, 255)

  // fringe removal
  bool remove_occluding_fringe = true;
  int occluding_erosion_px = 1; // @minmax(0, 20)
  bool remove_canny_fringe = false;
  int canny_proximity_px = 2; // @minmax(0, 20)
  int canny_erosion_px = 1;   // @minmax(0, 20)

  // mask smoothing
  int morph_close_radius = 2;    // @minmax(0, 10)
  int blur_radius = 3;           // @minmax(0, 10)
  float blur_threshold = 128.0f; // @minmax(1, 254)

  float removal_depth_range = 0.0f; // @minmax(0, 10)

  BackendType backend = BackendType::CPU;

  using OperatorType = FringeRemovalOperator;
  using Tag = rfl::Literal<"fringeRemoval">;
  static constexpr auto PluginName = "FringeRemovalOperator";
};

} // namespace pc::operators