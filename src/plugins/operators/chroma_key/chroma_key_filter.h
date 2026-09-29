#pragma once

#include <pointcaster/core_types.h>

#ifdef __CUDACC__
#define PC_CHROMA_KEY_FUNC __host__ __device__
#else
#define PC_CHROMA_KEY_FUNC
#endif

namespace pc::operators::chroma_key {

struct KeyParameters {
  float target_hue_degrees;
  float hue_width_degrees;
  float minimum_saturation;
  float minimum_value;
  bool invert;
};

// hue in degrees, saturation and value normalised
struct hsv_color {
  float h, s, v;
};

PC_CHROMA_KEY_FUNC inline hsv_color to_hsv(const color &col) {
  const float r = col.r / 255.f;
  const float g = col.g / 255.f;
  const float b = col.b / 255.f;

  const float max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const float min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const float delta = max_c - min_c;

  float hue = 0.f;
  if (delta >= 1e-5f) {
    if (max_c == r)
      hue = 60.f * ((g - b) / delta);
    else if (max_c == g)
      hue = 60.f * ((b - r) / delta + 2.f);
    else
      hue = 60.f * ((r - g) / delta + 4.f);
    if (hue < 0.f) hue += 360.f;
  }

  return {hue, max_c > 0.f ? delta / max_c : 0.f, max_c};
}

PC_CHROMA_KEY_FUNC inline bool in_key(const color &col,
                                      const KeyParameters &key) {
  const auto hsv = to_hsv(col);
  float hue_distance = hsv.h > key.target_hue_degrees
                           ? hsv.h - key.target_hue_degrees
                           : key.target_hue_degrees - hsv.h;
  if (hue_distance > 180.f) hue_distance = 360.f - hue_distance;
  return hue_distance <= key.hue_width_degrees * 0.5f &&
         hsv.s >= key.minimum_saturation && hsv.v >= key.minimum_value;
}

} // namespace pc::operators::chroma_key
