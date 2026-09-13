#pragma once

#include <pointcaster/core_types.h>
#include <rfl/DefaultVal.hpp>
#include <rfl/Literal.hpp>
#include <rfl/TaggedUnion.hpp>

namespace pc {

struct CodecConfiguration {
  enum class PointCloudCodec { None, Meshopt, Draco };

  struct NoCompressionConfiguration {
    using Tag = rfl::Literal<"none">;
  };

  struct MeshoptCompressionConfiguration {
    // 0 = fastest, 3 = smallest
    rfl::DefaultVal<int> level = 2; // @minmax(0, 3)
    using Tag = rfl::Literal<"meshopt">;
  };

  struct DracoCompressionConfiguration {
    // 0 = best compression
    // 10 = fastest
    // -1 lets draco decide
    rfl::DefaultVal<int> encode_speed = -1; // @minmax(-1, 10)
    rfl::DefaultVal<int> decode_speed = 10; // @minmax(-1, 10)
    // bits kept per float attribute value
    rfl::DefaultVal<Toggleable<int>> attribute_quantization = Toggleable<int>{false, 16}; // @minmax(1, 24)
    using Tag = rfl::Literal<"draco">;
  };

  using CompressionConfigurationVariant =
      rfl::TaggedUnion<"type", NoCompressionConfiguration,
                       MeshoptCompressionConfiguration,
                       DracoCompressionConfiguration>;

  rfl::DefaultVal<PointCloudCodec> codec = PointCloudCodec::Meshopt;
  rfl::DefaultVal<CompressionConfigurationVariant> compression = {
      MeshoptCompressionConfiguration{}};
};

} // namespace pc
