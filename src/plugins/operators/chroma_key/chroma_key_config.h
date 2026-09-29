#pragma once

#include <plugins/backend/backend_types.h>
#include <rfl/Literal.hpp>
#include <string>

namespace pc::operators {

class ChromaKeyOperator;

struct ChromaKeyConfiguration {
  std::string id;     // @hidden
  std::string label;  // @hidden
  bool active = true; // @hidden

  float target_hue_degrees = 140;   // @minmax(0, 360)
  float hue_width_degrees = 80;     // @minmax(0, 360)
  float minimum_saturation = 0.25f; // @minmax(0, 1)
  float minimum_value = 0.2f;       // @minmax(0, 1)

  bool invert = false;

  BackendType backend = BackendType::CPU;

  using OperatorType = ChromaKeyOperator;
  using Tag = rfl::Literal<"chromaKey">;
  static constexpr auto PluginName = "ChromaKeyOperator";
};

} // namespace pc::operators
