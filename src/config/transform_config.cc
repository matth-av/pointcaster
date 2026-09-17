#include "transform_config.h"

#include <core/util/geometry_utils.h>

namespace pc {

float4x4 to_float4x4(const TransformConfiguration &t) {
  // config translations are in metres, point positions are int16 millimetres
  static constexpr float metres_to_mm = 1000.f;
  const auto to_mm = [](const float3 &v) {
    return float3{v.x * metres_to_mm, v.y * metres_to_mm, v.z * metres_to_mm};
  };
  const float4x4 scale = scale_matrix(t.scale);
  const float4x4 rotate = rotation_matrix(t.rotation);
  const float4x4 translate = translation_matrix(to_mm(t.position));
  return multiply(translate, multiply(rotate, scale));
}

} // namespace pc