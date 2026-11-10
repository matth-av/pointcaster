#pragma once

#include "../backend_types.h"

#include <config/color_transform_config.h>
#include <config/transform_config.h>
#include <memory>
#include <pointcaster/point_cloud.h>

namespace pc::backend::cuda {

bool init_device_memory(const void *owner, const size_t point_count);
void free_device_memory(const void *owner);

void project_transform_frame_data(
    const void *owner, std::span<const uint16_t> input_depth_frame,
    std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
    const CameraIntrinsics &color_intrinsics,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform);

void project_transform_frame_data(
    const void *owner, std::span<const uint16_t> input_depth_frame,
    std::span<const color_bgra> input_bgra_frame, PointCloud &output_cloud,
    std::shared_ptr<const XyTable> xy_table,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform,
    const pc::float4x4 &world_transform = {});

BoundsFilterResult filter_to_bounds(const void *owner,
                                    const PointCloud &input_cloud,
                                    PointCloud &output_cloud,
                                    const position_bounds &bounds,
                                    BoundsFilterOptions options);

void transform_point_cloud(const void *owner, const PointCloud &input_cloud,
                           PointCloud &output_cloud,
                           const TransformConfiguration &transform,
                           const ColorTransformConfiguration &color_transform,
                           const pc::float4x4 &world_transform = {});

} // namespace pc::backend::cuda