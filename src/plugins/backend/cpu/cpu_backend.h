#pragma once

#include "../backend_plugin.h"

namespace pc::backend {

class CpuBackend : public BackendPlugin {
public:
  explicit CpuBackend(Corrade::PluginManager::AbstractManager &manager,
                      Corrade::Containers::StringView plugin);

  ~CpuBackend();

  CpuBackend(const CpuBackend &) = delete;
  CpuBackend &operator=(const CpuBackend &) = delete;
  CpuBackend(CpuBackend &&) = delete;
  CpuBackend &operator=(CpuBackend &&) = delete;

  void transform_point_cloud(
      const PointCloud &input_cloud, PointCloud &output_cloud,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform,
      const pc::float4x4 &world_transform = {}) const override;

  BoundsFilterResult
  filter_to_bounds(const PointCloud &input_cloud, PointCloud &output_cloud,
                   const position_bounds &bounds,
                   BoundsFilterOptions options) const override;

  void project_transform_frame_data(
      std::span<const uint16_t> input_depth_frame,
      std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
      const CameraIntrinsics &color_intrinsics,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform,
      const pc::float4x4 &world_transform,
      std::span<std::byte> render_output) const override;

  void project_transform_frame_data(
      std::span<const uint16_t> input_depth_frame,
      std::span<const color_bgra> input_bgra_frame, PointCloud &output_cloud,
      std::shared_ptr<const XyTable> xy_table,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform,
      const pc::float4x4 &world_transform,
      std::span<std::byte> render_output) const override;

  void pack_render_buffer(const PointCloud &cloud,
                          std::span<std::byte> output) const override;

  void project_frame(const PointCloud &cloud, camera::CameraFrameData &output,
                     camera::FrameProjectionArgs projection) const override;
};

} // namespace pc::backend