#pragma once

#include <config/attribute_config.h>
#include <config/color_transform_config.h>
#include <config/file_config.h>
#include <config/sequence_config.h>
#include <config/transform_config.h>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <plugins/operators/operator_variants.h>
#include <rfl/Literal.hpp>
#include <string>
#include <vector>

namespace pc::devices {

class PlyDevice;

struct PlyDeviceConfiguration {
  // Automatic reads floating point positions as metres and integer ones as
  // the millimetres they are already held in
  enum class PositionUnits { Automatic, Millimetres, Centimetres, Metres };

  std::string id; // @hidden

  std::string label;     // @hidden
  std::string parent_id; // @hidden
  int order = 0;         // @hidden

  FileFolderConfiguration file;
  PositionUnits position_units = PositionUnits::Automatic;
  SequenceConfiguration sequence; // @folded
  std::vector<AttributeConfiguration> attributes;
  TransformConfiguration transform;
  ColorTransformConfiguration color; // @folded

  operators::ConcurrentOperatorPipelineConfiguration
      operator_pipeline;                                          // @hidden
  std::vector<operators::OperatorConfigurationVariant> operators; // @hidden

  using DeviceType = PlyDevice;
  using Tag = rfl::Literal<"ply">;
  static constexpr auto PluginName = "PlyDevice";
};

} // namespace pc::devices