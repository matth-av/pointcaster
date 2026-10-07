#pragma once

#include <cstdint>
#include <glm/common.hpp>
#include <glm/gtc/noise.hpp>
#include <glm/vec3.hpp>
#include <pointcaster/core_types.h>

#ifdef __CUDACC__
#define PC_NOISE_FUNC __host__ __device__
#else
#define PC_NOISE_FUNC
#endif

namespace pc::operators::noise {

inline constexpr int max_octaves = 8;

enum class Algorithm { Simplex, Perlin, PeriodicPerlin };

struct DisplacementParameters {
  Algorithm algorithm;

  float magnitude; // millimetres of displacement for a noise value of 1
  int octaves;

  float frequencies[max_octaves]; // cycles per millimetre
  float amplitudes[max_octaves];
  float periods[max_octaves];

  glm::vec3 offsets[max_octaves][3];
};

PC_NOISE_FUNC inline float sample(const glm::vec3 &p, Algorithm algorithm,
                                  float period) {
  switch (algorithm) {
  case Algorithm::Perlin:
    return glm::perlin(p);
  case Algorithm::PeriodicPerlin:
    return glm::perlin(p, glm::vec3(period));
  default:
    return glm::simplex(p);
  }
}

// sum of octaves along one axis
PC_NOISE_FUNC inline float fractal(const glm::vec3 &p, int axis,
                                   const DisplacementParameters &params) {
  float sum = 0.f;
  for (int octave = 0; octave < params.octaves; ++octave) {
    const auto field_position =
        p * params.frequencies[octave] + params.offsets[octave][axis];
    sum += params.amplitudes[octave] *
           sample(field_position, params.algorithm, params.periods[octave]);
  }
  return sum;
}

PC_NOISE_FUNC inline position displace(const position &point,
                                       const DisplacementParameters &params) {
  const glm::vec3 original(point.x, point.y, point.z);

  const glm::vec3 offset(fractal(original, 0, params),
                         fractal(original, 1, params),
                         fractal(original, 2, params));

  // back onto whole millimetres, held inside what a position can store
  const glm::vec3 displaced =
      glm::clamp(glm::round(original + offset * params.magnitude),
                 glm::vec3(-32768.f), glm::vec3(32767.f));

  return {static_cast<int16_t>(displaced.x), static_cast<int16_t>(displaced.y),
          static_cast<int16_t>(displaced.z)};
}

} // namespace pc::operators::noise
