#pragma once

#include <cstdint>
#include <pointcaster/point_cloud.h>
#include <string>
#include <string_view>
#include <type_traits>

namespace pc {

// the cloud attribute an imported per-point property is read into...
// atm its only the PointScale
enum class AttributeTarget { None, PointScale };

enum class AttributePrecision { Full, Bits16, Bits8 };

// how one of a source's per-point properties is imported
struct AttributeConfiguration {
  std::string name; // @hidden

  AttributeTarget target = AttributeTarget::None;
  AttributePrecision precision = AttributePrecision::Full;
  float range_min = 0.0f;
  float range_max = 1.0f;

  bool operator==(const AttributeConfiguration &) const = default;
};

// empty for None, which names nothing in the cloud
constexpr std::string_view cloud_attribute_name(AttributeTarget target) {
  switch (target) {
  case AttributeTarget::PointScale:
    return point_scale_attribute;
  case AttributeTarget::None:
    return {};
  }
  return {};
}

constexpr void visit_raw_type(const AttributeConfiguration &configuration,
                              auto &&visit) {
  const bool signed_raw_type = configuration.range_min < 0.0f;
  switch (configuration.precision) {
  case AttributePrecision::Full:
    return;
  case AttributePrecision::Bits16:
    if (signed_raw_type) return visit(std::type_identity<int16_t>{});
    return visit(std::type_identity<uint16_t>{});
  case AttributePrecision::Bits8:
    if (signed_raw_type) return visit(std::type_identity<int8_t>{});
    return visit(std::type_identity<uint8_t>{});
  }
}

} // namespace pc
