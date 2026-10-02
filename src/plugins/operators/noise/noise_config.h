#pragma once

#include <config/core_types_reflection.h>
#include <plugins/backend/backend_types.h>
#include <pointcaster/core_types.h>
#include <rfl/Literal.hpp>
#include <string>

namespace pc::operators {

class NoiseOperator;

struct NoiseConfiguration {
  std::string id;     // @hidden
  std::string label;  // @hidden
  bool active = true; // @hidden

  enum class Algorithm { Simplex, Perlin };

  Algorithm algorithm = Algorithm::Simplex;

  length feature_size = length{1000}; // @minmax(10, 10000)
  length magnitude = length{400};     // @minmax(0, 2000)
  int seed = 99;                      // @minmax(0, 1000)

  int octaves = 4;         // @minmax(1, 8)
  float lacunarity = 2.0f; // @minmax(1, 4)
  float decay = 0.5f;      // @minmax(0, 1)

  // ignored by simplex...
  Toggleable<int> period = Toggleable<int>{false, 4}; // @minmax(1, 64)

  BackendType backend = BackendType::CPU;

  using OperatorType = NoiseOperator;
  using Tag = rfl::Literal<"noise">;
  static constexpr auto PluginName = "NoiseOperator";
};

} // namespace pc::operators
