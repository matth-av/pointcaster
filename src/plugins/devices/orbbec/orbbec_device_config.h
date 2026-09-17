#pragma once

#include <config/color_transform_config.h>
#include <config/core_types_reflection.h>
#include <config/network_config.h>
#include <config/transform_config.h>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <plugins/operators/operator_variants.h>
#include <pointcaster/point_cloud.h>
#include <rfl/Literal.hpp>
#include <rfl/TaggedUnion.hpp>
#include <rfl/Variant.hpp>
#include <string>

namespace pc::devices {

class OrbbecDevice;

struct OrbbecDeviceConfiguration {
  std::string id;     // @hidden
  std::string serial; // @disabled
  std::string label;  // @hidden

  std::string parent_id; // @hidden
  int order = 0;         // @hidden

  // enumerations shared among sensor configuration types

  enum class DepthMode { Narrow, Wide };
  enum class AcquisitionMode { XYZRGB, XYZ };
  enum class PointConversionMode { D2C, C2D };
  enum class ColorResolution {
    HD_1280x720,
    QuadVGA_1280x960,
    FHD_1920x1080,
    QHD_2560x1440,
    UHD_3840x2160
  };
  enum class DepthResolution {
    NFOV_320x288,
    WFOV_512x512,
    NFOV_640x576,
    WFOV_1024x1024
  };
  enum class SyncMode { Standalone, Software };
  enum class ColorMapping { LogIntensity, LinearIntensity, Depth, Solid };

  // the different sensor types that the orbbec driver supports have different
  // configuration shapes and defaults

  struct RgbdSensorConfiguration {
    DepthMode depth_mode = DepthMode::Narrow;
    AcquisitionMode acquisition_mode = AcquisitionMode::XYZRGB;
    PointConversionMode conversion_mode = PointConversionMode::D2C;
    ColorResolution color_resolution = ColorResolution::HD_1280x720;
    DepthResolution depth_resolution = DepthResolution::NFOV_640x576;
    SyncMode sync_mode = SyncMode::Standalone;

    // resolved from the stream profiles once the pipeline starts
    rfl::Skip<int> fps; // @disabled

    using Tag = rfl::Literal<"rgbd">;
  };

  struct LidarSensorConfiguration {
    uint32_t scan_rate = 30; // @options(15, 20, 25, 30, 40) @suffix(Hz)

    Toggleable<radius> maximum_distance = Toggleable<radius>{
        .active = false, .value = radius{1500}}; // @minmax(50, 65535)

    ColorMapping color_mapping = ColorMapping::LogIntensity;

    using Tag = rfl::Literal<"lidar">;
  };

  using SensorConfigurationVariant =
      rfl::TaggedUnion<"type", RgbdSensorConfiguration,
                       LidarSensorConfiguration>;

  SensorConfigurationVariant sensor = {RgbdSensorConfiguration{}};

  NetworkConfiguration network; // @folded
  TransformConfiguration transform;
  ColorTransformConfiguration color; // @folded

  operators::ConcurrentOperatorPipelineConfiguration
      operator_pipeline;                                          // @hidden
  std::vector<operators::OperatorConfigurationVariant> operators; // @hidden

  using DeviceType = OrbbecDevice;
  using Tag = rfl::Literal<"orbbec">;
  static constexpr auto PluginName = "OrbbecDevice";
};

namespace orbbec {

using ColorResolution = OrbbecDeviceConfiguration::ColorResolution;
using DepthResolution = OrbbecDeviceConfiguration::DepthResolution;

inline static pc::int2 resolution(ColorResolution resolution_enum) {
  if (resolution_enum == ColorResolution::HD_1280x720) return {1280, 720};
  if (resolution_enum == ColorResolution::FHD_1920x1080) return {1920, 1080};
  return {};
};

inline static pc::int2 resolution(DepthResolution resolution_enum) {
  if (resolution_enum == DepthResolution::NFOV_320x288) return {320, 288};
  if (resolution_enum == DepthResolution::WFOV_512x512) return {512, 512};
  if (resolution_enum == DepthResolution::NFOV_640x576) return {640, 576};
  if (resolution_enum == DepthResolution::WFOV_1024x1024) return {1024, 1024};
  return {};
};

} // namespace orbbec

} // namespace pc::devices