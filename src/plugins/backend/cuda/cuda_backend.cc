#include "cuda_backend.h"
#include "cuda_backend_core.h"

#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <core/logger/logger.h>
#include <cstdint>
#include <exception>

namespace pc::backend {

void CudaBackend::init(const size_t point_count) {
  if (!cuda::init_device_memory(this, point_count)) {
    pc::logger()->error("Failed to initialse CUDA device memory");
  }
};

CudaBackend::~CudaBackend() {
  cuda::free_device_memory(this);
};

void CudaBackend::project_transform_frame_data(
    std::span<const uint16_t> input_depth_frame,
    std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
    const CameraIntrinsics &color_intrinsics,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform, const pc::float4x4 &,
    std::span<std::byte> render_output) const {
  try {
    cuda::project_transform_frame_data(
        this, input_depth_frame, input_rgb_frame, output_cloud,
        color_intrinsics, transform, color_transform, render_output);
  } catch (const std::exception &e) {
    pc::logger()->error("CUDA backend error: {}", e.what());
  } catch (...) {
    pc::logger()->error("CUDA backend error: Unknown exception");
  }
}

void CudaBackend::transform_point_cloud(
    const PointCloud &input_cloud, PointCloud &output_cloud,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform,
    const pc::float4x4 &world_transform) const {
  try {
    cuda::transform_point_cloud(this, input_cloud, output_cloud, transform,
                                color_transform, world_transform);
  } catch (const std::exception &e) {
    pc::logger()->error("CUDA backend error: {}", e.what());
  } catch (...) {
    pc::logger()->error("CUDA backend error: Unknown exception");
  }
}

BoundsFilterResult CudaBackend::filter_to_bounds(
    const PointCloud &input_cloud, PointCloud &output_cloud,
    const position_bounds &bounds, BoundsFilterOptions options) const {
  try {
    return cuda::filter_to_bounds(this, input_cloud, output_cloud, bounds,
                                  options);
  } catch (const std::exception &e) {
    pc::logger()->error("CUDA backend error: {}", e.what());
  } catch (...) {
    pc::logger()->error("CUDA backend error: Unknown exception");
  }
  // failed pass
  return {.input_count = input_cloud.size()};
}

void CudaBackend::pack_render_buffer(const PointCloud &,
                                     std::span<std::byte>) const {
  // void CudaBackend::pack_render_buffer(const PointCloud &cloud,
  //                                      std::span<std::byte> output) const {
  //...
  pc::logger()->error("Unimplemented CUDA backend function");
};

void CudaBackend::project_frame(const PointCloud &, camera::CameraFrameData &,
                                camera::FrameProjectionArgs) const {
  // void project_frame(const PointCloud &cloud, camera::CameraFrameData
  // &output,
  //                    camera::FrameProjectionArgs projection) const {
  //...
  pc::logger()->error("Unimplemented CUDA backend function");
};

} // namespace pc::backend

CORRADE_PLUGIN_REGISTER(CudaBackend, pc::backend::CudaBackend,
                        "net.pointcaster.BackendPlugin/1.0")