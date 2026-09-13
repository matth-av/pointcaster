#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace pc {

using int2 = std::pair<int, int>;
using float2 = std::pair<float, float>;

struct float3 {
  float x = 0;
  float y = 0;
  float z = 0;
  auto operator<=>(const float3 &f) const = default;
};

struct float4 {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 0;
  auto operator<=>(const float4 &f) const = default;
};

struct float4x4 {
  std::array<float, 16> values{
      1, 0, 0, 0, //
      0, 1, 0, 0, //
      0, 0, 1, 0, //
      0, 0, 0, 1,
  };
  bool operator==(const float4x4 &) const = default;
};

struct quaternion {
  float scalar = 1;
  float x = 0;
  float y = 0;
  float z = 0;
  auto operator<=>(const quaternion &q) const = default;
};

struct uint2 {
  unsigned int x = 0, y = 0;
  auto operator<=>(const uint2 &u) const = default;
};

struct alignas(4) short3 {
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
  int16_t __pad = 0;
  auto operator<=>(const short3 &s) const = default;
};

struct alignas(4) position {
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
  int16_t __pad = 0;
  auto operator<=>(const position &p) const = default;
};

struct position_bounds {
  position min{std::numeric_limits<int16_t>::max(),
               std::numeric_limits<int16_t>::max(),
               std::numeric_limits<int16_t>::max()};
  position max{std::numeric_limits<int16_t>::min(),
               std::numeric_limits<int16_t>::min(),
               std::numeric_limits<int16_t>::min()};

  bool operator==(const position_bounds &b) const = default;

  // grow to contain a position
  constexpr void encompass(const position &p) {
    if (p.x < min.x) min.x = p.x;
    if (p.y < min.y) min.y = p.y;
    if (p.z < min.z) min.z = p.z;
    if (p.x > max.x) max.x = p.x;
    if (p.y > max.y) max.y = p.y;
    if (p.z > max.z) max.z = p.z;
  }
};

inline constexpr position_bounds default_config_bounds{{-2500, 0, -2500},
                                                       {2500, 2500, 2500}};

// a scalar distance in the int16 millimetre space...
// we tag it as a "length" instead of a bare int16_t
// for a bit more static info, along with some helpers for converting units
struct length {
  int16_t mm = 0;
  auto operator<=>(const length &l) const = default;

  constexpr float metres() const { return mm / 1000.0f; }
  constexpr float centimetres() const { return mm / 10.0f; }

  static constexpr length from_millimetres(float millimetres) {
    constexpr auto lowest =
        static_cast<float>(std::numeric_limits<int16_t>::min());
    constexpr auto highest =
        static_cast<float>(std::numeric_limits<int16_t>::max());
    const float rounded =
        millimetres < 0 ? millimetres - 0.5f : millimetres + 0.5f;
    if (rounded <= lowest) return length{std::numeric_limits<int16_t>::min()};
    if (rounded >= highest) return length{std::numeric_limits<int16_t>::max()};
    return length{static_cast<int16_t>(rounded)};
  }

  static constexpr length from_metres(float metres) {
    return from_millimetres(metres * 1000.0f);
  }

  static constexpr length from_centimetres(float centimetres) {
    return from_millimetres(centimetres * 10.0f);
  }
};

// an unsigned distance out from a point, stored as an unsigned millimeter
struct radius {
  uint16_t mm = 0;
  auto operator<=>(const radius &r) const = default;

  constexpr float metres() const { return mm / 1000.0f; }
  constexpr float centimetres() const { return mm / 10.0f; }

  static constexpr radius from_millimetres(float millimetres) {
    constexpr auto highest =
        static_cast<float>(std::numeric_limits<uint16_t>::max());
    const float rounded = millimetres + 0.5f;
    if (rounded <= 0) return radius{0};
    if (rounded >= highest) return radius{std::numeric_limits<uint16_t>::max()};
    return radius{static_cast<uint16_t>(rounded)};
  }

  static constexpr radius from_metres(float metres) {
    return from_millimetres(metres * 1000.0f);
  }

  static constexpr radius from_centimetres(float centimetres) {
    return from_millimetres(centimetres * 10.0f);
  }
};

// a float value quantised into an integer..
// the storage type sets the range and whether it is signed,
// the step sets the resolution
template <std::integral Storage, float step> struct basic_scale {
  static_assert(step > 0.0f,
                "step is the gap between two representable values");

  using storage_type = Storage;
  static constexpr float step_value = step;

  storage_type raw = 0;
  auto operator<=>(const basic_scale &s) const = default;

  constexpr float value() const { return static_cast<float>(raw) * step; }

  // rounds to the nearest step and clamps to what the storage can hold
  static constexpr basic_scale from_value(float value) {
    constexpr auto lowest =
        static_cast<float>(std::numeric_limits<storage_type>::lowest());
    constexpr auto highest =
        static_cast<float>(std::numeric_limits<storage_type>::max());
    const float steps = value / step;
    const float rounded = steps < 0 ? steps - 0.5f : steps + 0.5f;
    if (rounded <= lowest) {
      return basic_scale{std::numeric_limits<storage_type>::lowest()};
    }
    if (rounded >= highest) {
      return basic_scale{std::numeric_limits<storage_type>::max()};
    }
    return basic_scale{static_cast<storage_type>(rounded)};
  }
};

// a millimetre of resolution across 0 .. 65.535 m. two bytes
using scale = basic_scale<uint16_t, 0.001f>;

// a value the configuration can switch off without losing what it holds
template <class T> struct Toggleable {
  using value_type = T;

  bool active = false;
  T value{};

  bool operator==(const Toggleable &t) const = default;
};

inline constexpr Toggleable<position_bounds> default_crop_bounds{
    false, default_config_bounds};

struct color {
  unsigned char r = 0, g = 0, b = 0, a = 0;
  auto operator<=>(const color &c) const = default;
};

struct color_rgb {
  unsigned char r = 0;
  unsigned char g = 0;
  unsigned char b = 0;
  auto operator<=>(const color_rgb &c) const = default;

  operator color() const { return {r, g, b, 255}; };
};

} // namespace pc