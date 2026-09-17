#pragma once

#include <pointcaster/core_types.h>

namespace pc {

struct ColorTransformConfiguration {
  float gain = 1; // @minmax(0, 15)
};

} // namespace pc