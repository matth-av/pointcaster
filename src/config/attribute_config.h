#pragma once

#include <cstdint>
#include <pointcaster/point_cloud.h>
#include <rfl/DefaultVal.hpp>
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

  rfl::DefaultVal<AttributeTarget> target = AttributeTarget::None;
  rfl::DefaultVal<AttributePrecision> precision = AttributePrecision::Full;
  rfl::DefaultVal<float> range_min = 0.0f;
  rfl::DefaultVal<float> range_max = 1.0f;
};

inline bool operator==(const AttributeConfiguration &a,
                       const AttributeConfiguration &b) {
  return a.name == b.name && a.target.value() == b.target.value() &&
         a.precision.value() == b.precision.value() &&
         a.range_min.value() == b.range_min.value() &&
         a.range_max.value() == b.range_max.value();
}

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
  const bool signed_raw_type = configuration.range_min.value() < 0.0f;
  switch (configuration.precision.value()) {
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
