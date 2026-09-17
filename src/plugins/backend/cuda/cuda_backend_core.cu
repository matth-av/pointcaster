#include "cuda_backend_core.h"

#include "../backend_filters.h"
#include "../backend_utils.h"

#include <config/transform_config.h>
#include <mutex>
#include <pointcaster/core_types.h>
#include <profiling/profiling_zone.h>
#include <stdexcept>
#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/fill.h>
#include <thrust/host_vector.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/transform_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/partition.h>
#include <thrust/sequence.h>
#include <thrust/system/tbb/execution_policy.h>
#include <unordered_map>
#include <vector>

namespace {

using namespace pc;

struct DeviceTransformMemory {
  size_t point_count;
  thrust::device_vector<uint16_t> input_depth_data;
  thrust::device_vector<color_rgb> input_rgb_data;
  thrust::device_vector<position> input_positions;
  thrust::device_vector<color> input_colors;
  thrust::device_vector<position> output_positions;
  thrust::device_vector<color> output_colors;
  thrust::device_vector<uint32_t> output_indices;
  thrust::device_vector<std::byte> interleaved_render;
  thrust::device_vector<int> indices;

  void ensure_capacity(size_t n) {
    if (n <= point_count) return;
    input_depth_data.resize(n);
    input_rgb_data.resize(n);
    input_positions.resize(n);
    input_colors.resize(n);
    output_positions.resize(n);
    output_colors.resize(n);
    output_indices.resize(n);
    interleaved_render.resize(n * 16);
    indices.resize(n);
    thrust::sequence(indices.begin(), indices.end());
    point_count = n;
  }
};

std::mutex device_memory_access;
std::unordered_map<const void *, DeviceTransformMemory> instance_device_memory;

void create_device_memory(const void *owner, const size_t point_count) {
  DeviceTransformMemory new_device_memory{
      .point_count = point_count,
      .input_depth_data = thrust::device_vector<uint16_t>(point_count),
      .input_rgb_data = thrust::device_vector<color_rgb>(point_count),
      .input_positions = thrust::device_vector<position>(point_count),
      .input_colors = thrust::device_vector<color>(point_count),
      .output_positions = thrust::device_vector<position>(point_count),
      .output_colors = thrust::device_vector<color>(point_count),
      .output_indices = thrust::device_vector<uint32_t>(point_count),
      .interleaved_render = thrust::device_vector<std::byte>(point_count * 16),
      .indices = thrust::device_vector<int>(point_count)};

  thrust::sequence(new_device_memory.indices.begin(),
                   new_device_memory.indices.end());

  std::lock_guard lock(device_memory_access);
  auto [entry, inserted] =
      instance_device_memory.try_emplace(owner, std::move(new_device_memory));
  if (!inserted) entry->second.ensure_capacity(point_count);
}

} // namespace

namespace pc::backend::cuda {

struct ProjectAndTransform {

  CameraIntrinsics color_intrinsics;
  filter::TransformFilterParameters params;
  bool sample_cloud;

  explicit ProjectAndTransform(
      const CameraIntrinsics &intrinsics,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform)
      : color_intrinsics(intrinsics) {
    params = filter::TransformFilterParameters::from_config(transform,
                                                            color_transform);
    sample_cloud = params.sample > 1;
  }

  using OutputPointT = thrust::tuple<position, color>;
  using InputPixelT = thrust::tuple<uint16_t, color_rgb, int>;

  __host__ __device__ OutputPointT operator()(InputPixelT input) const {
    const int i = thrust::get<2>(input);
    if (sample_cloud && !filter::sample(i, params)) {
      return thrust::make_tuple(filter::invalid_position_value, color{});
    }
    const uint16_t depth = thrust::get<0>(input);
    const color_rgb rgb = thrust::get<1>(input);
    const auto &frame_width = color_intrinsics.frame_width;

    const auto px = i % frame_width;
    const auto py = i / frame_width;

    auto position = util::project_2d_to_3d(px, py, depth, color_intrinsics);
    position = filter::transform(position, params);

    auto col = filter::color_transform(color{rgb.r, rgb.g, rgb.b}, params);

    return thrust::make_tuple(position, col);
  }
};

struct BoundsCheck {
  filter::TransformFilterParameters params;

  template <typename PointT>
  __host__ __device__ bool operator()(const PointT &point) const {
    auto pos = thrust::get<0>(point);
    return filter::is_valid(pos) && filter::in_bounds(pos, params);
  }
};

struct InterleaveForRender {
  const position *positions;
  const color *colors;
  std::byte *output;

  __host__ __device__ void operator()(int i) const {
    memcpy(output + i * 16, &positions[i], 8);
    memcpy(output + i * 16 + 8, &colors[i], 4);
    float idx = static_cast<float>(i);
    memcpy(output + i * 16 + 12, &idx, 4);
  }
};

struct PositionAsBounds {
  __host__ __device__ position_bounds operator()(const position &p) const {
    return filter::as_bounds(p);
  }
};

struct MergeBounds {
  __host__ __device__ position_bounds
  operator()(const position_bounds &a, const position_bounds &b) const {
    return filter::merge_bounds(a, b);
  }
};

__host__ __device__ inline position invalid_position() {
  return position{filter::invalid_position_value.x,
                  filter::invalid_position_value.y,
                  filter::invalid_position_value.z};
}

struct TransformCloudPoint {
  filter::TransformFilterParameters params;
  bool sample_cloud;

  explicit TransformCloudPoint(
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform) {
    params = filter::TransformFilterParameters::from_config(transform,
                                                            color_transform);
    sample_cloud = params.sample > 1;
  }

  using OutputPointT = thrust::tuple<position, color, uint32_t>;
  using InputPointT = thrust::tuple<position, color, int>;

  __host__ __device__ OutputPointT operator()(InputPointT input) const {
    const int i = thrust::get<2>(input);
    const auto index = static_cast<uint32_t>(i);
    if (sample_cloud && !filter::sample(i, params)) {
      return thrust::make_tuple(invalid_position(), color{}, index);
    }
    auto pos = filter::transform(thrust::get<0>(input), params);
    auto col = filter::color_transform(thrust::get<1>(input), params);
    return thrust::make_tuple(pos, col, index);
  }
};

struct ApplyWorldTransform {
  float w[12];

  explicit ApplyWorldTransform(const pc::float4x4 &world) {
    for (int i = 0; i < 12; ++i) w[i] = world.values[i];
  }

  __host__ __device__ position operator()(const position &p) const {
    const float x = float(p.x), y = float(p.y), z = float(p.z);
    return position{
        static_cast<int16_t>(lroundf(w[0] * x + w[1] * y + w[2] * z + w[3])),
        static_cast<int16_t>(lroundf(w[4] * x + w[5] * y + w[6] * z + w[7])),
        static_cast<int16_t>(lroundf(w[8] * x + w[9] * y + w[10] * z + w[11])),
    };
  }
};

// the same box test both backends share, wrapped so thrust can carry it
struct InBoundsCheck {
  position_bounds bounds;
  bool invert;

  __host__ __device__ bool operator()(const position &p) const {
    return filter::in_bounds(p, bounds) != invert;
  }
};

struct InBoundsPointCheck {
  InBoundsCheck keeps_point;

  __host__ __device__ bool
  operator()(thrust::tuple<position, color, uint32_t> point) const {
    return keeps_point(thrust::get<0>(point));
  }
};

struct PositionAsMeasurementIfKept {
  InBoundsCheck keeps_point;

  __host__ __device__ filter::bounds_measurement
  operator()(const position &p) const {
    return keeps_point(p) ? filter::as_measurement(p)
                          : filter::empty_measurement();
  }
};

struct MergeMeasurements {
  __host__ __device__ filter::bounds_measurement
  operator()(const filter::bounds_measurement &a,
             const filter::bounds_measurement &b) const {
    return filter::merge_measurements(a, b);
  }
};

bool init_device_memory(const void *owner, const size_t point_count) {
  try {
    create_device_memory(owner, point_count);
    return true;
  } catch (...) {
  }
  return false;
}

void free_device_memory(const void *owner) {
  instance_device_memory.erase(owner);
}

void project_transform_frame_data(
    const void *owner, std::span<const uint16_t> input_depth_frame,
    std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
    const CameraIntrinsics &color_intrinsics,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform,
    std::span<std::byte> render_output) {

  DeviceTransformMemory *device_memory;
  {
    std::lock_guard lock(device_memory_access);
    auto it = instance_device_memory.find(owner);
    if (it == instance_device_memory.end())
      throw std::runtime_error("device memory is not initialised");
    device_memory = &it->second;
  }

  using namespace pc::profiling;

  const size_t point_count = input_depth_frame.size();
  device_memory->ensure_capacity(point_count);

  const auto transform_parameters =
      filter::TransformFilterParameters::from_config(transform,
                                                     color_transform);

  bool output_render_buffer = !render_output.empty();

  {
    ProfilingZone copy_zone("CudaBackend::copy_to_device");
    thrust::copy(input_depth_frame.begin(), input_depth_frame.end(),
                 device_memory->input_depth_data.begin());
    thrust::copy(input_rgb_frame.begin(), input_rgb_frame.end(),
                 device_memory->input_rgb_data.begin());
  }

  // zip together our input depth, colour and indices for use inside a single
  // algorithm
  auto frame_data_input_begin = thrust::make_zip_iterator(thrust::make_tuple(
      device_memory->input_depth_data.begin(),
      device_memory->input_rgb_data.begin(), device_memory->indices.begin()));
  auto frame_data_input_end = frame_data_input_begin + point_count;

  // similarly zip together our output destination memory so the SoA output is
  // available in the single kernel
  auto output_points_begin = thrust::make_zip_iterator(
      thrust::make_tuple(device_memory->output_positions.begin(),
                         device_memory->output_colors.begin()));

  size_t new_point_count;
  position_bounds new_cloud_bounds;

  {
    ProfilingZone transform_zone("CudaBackend::transform_and_filter");

    thrust::transform(
        thrust::cuda::par, frame_data_input_begin, frame_data_input_end,
        output_points_begin,
        ProjectAndTransform{color_intrinsics, transform, color_transform});

    auto output_points_end = output_points_begin + point_count;

    auto new_end =
        thrust::partition(thrust::cuda::par, output_points_begin,
                          output_points_end, BoundsCheck{transform_parameters});

    new_point_count = new_end - output_points_begin;

    new_cloud_bounds = thrust::transform_reduce(
        thrust::cuda::par, device_memory->output_positions.begin(),
        device_memory->output_positions.begin() + new_point_count,
        PositionAsBounds{}, position_bounds{}, MergeBounds{});

    if (output_render_buffer) {
      // reset indices to sequential for the filtered range
      thrust::sequence(device_memory->indices.begin(),
                       device_memory->indices.begin() + new_point_count);

      thrust::for_each(
          thrust::cuda::par, device_memory->indices.begin(),
          device_memory->indices.begin() + new_point_count,
          InterleaveForRender{
              thrust::raw_pointer_cast(device_memory->output_positions.data()),
              thrust::raw_pointer_cast(device_memory->output_colors.data()),
              thrust::raw_pointer_cast(
                  device_memory->interleaved_render.data())});
    }
  }

  {
    ProfilingZone output_zone("CudaBackend::copy_back_to_host");

    thrust::copy(device_memory->output_positions.begin(),
                 device_memory->output_positions.begin() + new_point_count,
                 output_cloud.positions.begin());
    thrust::copy(device_memory->output_colors.begin(),
                 device_memory->output_colors.begin() + new_point_count,
                 output_cloud.colors.begin());

    output_cloud.bounds = new_cloud_bounds;

    if (output_render_buffer) {
      thrust::copy(device_memory->interleaved_render.begin(),
                   device_memory->interleaved_render.begin() +
                       new_point_count * 16,
                   render_output.begin());
    }
  }

  output_cloud.resize(new_point_count);
}

BoundsFilterResult filter_to_bounds(const void *owner,
                                    const PointCloud &input_cloud,
                                    PointCloud &output_cloud,
                                    const position_bounds &bounds,
                                    BoundsFilterOptions options) {

  DeviceTransformMemory *device_memory;
  {
    std::lock_guard lock(device_memory_access);
    auto it = instance_device_memory.find(owner);
    if (it == instance_device_memory.end())
      throw std::runtime_error("device memory is not initialised");
    device_memory = &it->second;
  }

  using namespace pc::profiling;

  const size_t input_count = input_cloud.size();
  BoundsFilterResult result{.input_count = input_count};
  if (input_count == 0) return result;

  device_memory->ensure_capacity(input_count);

  const bool write_output = !options.analyse_only;
  const InBoundsCheck keeps_point{bounds, options.invert};

  {
    ProfilingZone copy_zone("CudaBackend::copy_to_device");
    thrust::copy(input_cloud.positions.begin(),
                 input_cloud.positions.begin() + input_count,
                 device_memory->input_positions.begin());
    if (write_output) {
      thrust::copy(input_cloud.colors.begin(),
                   input_cloud.colors.begin() + input_count,
                   device_memory->input_colors.begin());
    }
  }

  if (!write_output) {
    ProfilingZone measure_zone("CudaBackend::measure_in_bounds");
    const auto measured = thrust::transform_reduce(
        thrust::cuda::par, device_memory->input_positions.begin(),
        device_memory->input_positions.begin() + input_count,
        PositionAsMeasurementIfKept{keeps_point}, filter::empty_measurement(),
        MergeMeasurements{});
    result.point_count = measured.point_count;
    result.bounds = measured.bounds;
    return result;
  }

  size_t kept_count;

  {
    ProfilingZone filter_zone("CudaBackend::filter_to_bounds");

    auto input_points_begin = thrust::make_zip_iterator(
        thrust::make_tuple(device_memory->input_positions.begin(),
                           device_memory->input_colors.begin(),
                           thrust::make_counting_iterator(uint32_t{0})));
    auto output_points_begin = thrust::make_zip_iterator(
        thrust::make_tuple(device_memory->output_positions.begin(),
                           device_memory->output_colors.begin(),
                           device_memory->output_indices.begin()));

    auto new_end = thrust::copy_if(
        thrust::cuda::par, input_points_begin, input_points_begin + input_count,
        output_points_begin, InBoundsPointCheck{keeps_point});
    kept_count = static_cast<size_t>(new_end - output_points_begin);
  }

  result.point_count = kept_count;
  if (kept_count == 0) return result;

  {
    ProfilingZone bounds_zone("CudaBackend::measure_bounds");
    result.bounds = thrust::transform_reduce(
        thrust::cuda::par, device_memory->output_positions.begin(),
        device_memory->output_positions.begin() + kept_count,
        PositionAsBounds{}, filter::empty_bounds(), MergeBounds{});
  }

  {
    ProfilingZone output_zone("CudaBackend::copy_back_to_host");

    // the index list only has to come back off the gpu when there are
    // attributes for it to place
    std::vector<uint32_t> kept_indices;
    if (!input_cloud.attributes.empty()) {
      kept_indices.resize(kept_count);
      thrust::copy(device_memory->output_indices.begin(),
                   device_memory->output_indices.begin() + kept_count,
                   kept_indices.begin());
    }

    input_cloud.gather_attributes_into(output_cloud, kept_indices);

    output_cloud.resize(kept_count);
    thrust::copy(device_memory->output_positions.begin(),
                 device_memory->output_positions.begin() + kept_count,
                 output_cloud.positions.begin());
    thrust::copy(device_memory->output_colors.begin(),
                 device_memory->output_colors.begin() + kept_count,
                 output_cloud.colors.begin());
    output_cloud.bounds = result.bounds;
  }

  return result;
}

void transform_point_cloud(const void *owner, const PointCloud &input_cloud,
                           PointCloud &output_cloud,
                           const TransformConfiguration &transform,
                           const ColorTransformConfiguration &color_transform,
                           const pc::float4x4 &world_transform) {

  DeviceTransformMemory *device_memory;
  {
    std::lock_guard lock(device_memory_access);
    auto it = instance_device_memory.find(owner);
    if (it == instance_device_memory.end())
      throw std::runtime_error("device memory is not initialised");
    device_memory = &it->second;
  }

  const size_t point_count = input_cloud.size();
  device_memory->ensure_capacity(point_count);

  using namespace pc::profiling;

  const auto transform_parameters =
      filter::TransformFilterParameters::from_config(transform,
                                                     color_transform);

  {
    ProfilingZone copy_zone("CudaBackend::copy_to_device");
    thrust::copy(input_cloud.positions.begin(),
                 input_cloud.positions.begin() + point_count,
                 device_memory->input_positions.begin());
    thrust::copy(input_cloud.colors.begin(),
                 input_cloud.colors.begin() + point_count,
                 device_memory->input_colors.begin());
  }

  auto input_points_begin = thrust::make_zip_iterator(thrust::make_tuple(
      device_memory->input_positions.begin(),
      device_memory->input_colors.begin(), device_memory->indices.begin()));

  auto output_points_begin = thrust::make_zip_iterator(
      thrust::make_tuple(device_memory->output_positions.begin(),
                         device_memory->output_colors.begin(),
                         device_memory->output_indices.begin()));

  size_t new_point_count;
  position_bounds new_cloud_bounds;

  {
    ProfilingZone transform_zone("CudaBackend::transform_and_filter");

    thrust::transform(thrust::cuda::par, input_points_begin,
                      input_points_begin + point_count, output_points_begin,
                      TransformCloudPoint{transform, color_transform});

    auto output_points_end = output_points_begin + point_count;
    auto new_end =
        thrust::partition(thrust::cuda::par, output_points_begin,
                          output_points_end, BoundsCheck{transform_parameters});
    new_point_count = new_end - output_points_begin;

    const bool has_world_transform = !(world_transform == pc::float4x4{});
    if (has_world_transform) {
      thrust::transform(
          thrust::cuda::par, device_memory->output_positions.begin(),
          device_memory->output_positions.begin() + new_point_count,
          device_memory->output_positions.begin(),
          ApplyWorldTransform{world_transform});
    }

    new_cloud_bounds = thrust::transform_reduce(
        thrust::cuda::par, device_memory->output_positions.begin(),
        device_memory->output_positions.begin() + new_point_count,
        PositionAsBounds{}, position_bounds{}, MergeBounds{});
  }

  {
    ProfilingZone output_zone("CudaBackend::copy_back_to_host");
    thrust::copy(device_memory->output_positions.begin(),
                 device_memory->output_positions.begin() + new_point_count,
                 output_cloud.positions.begin());
    thrust::copy(device_memory->output_colors.begin(),
                 device_memory->output_colors.begin() + new_point_count,
                 output_cloud.colors.begin());
    output_cloud.bounds = new_cloud_bounds;
  }

  std::vector<uint32_t> kept_indices;
  if (!input_cloud.attributes.empty()) {
    kept_indices.resize(new_point_count);
    thrust::copy(device_memory->output_indices.begin(),
                 device_memory->output_indices.begin() + new_point_count,
                 kept_indices.begin());
  }

  input_cloud.gather_attributes_into(output_cloud, kept_indices);

  output_cloud.resize(new_point_count);

  const auto &point_scale = transform.point_scale;
  if (point_scale.active) {
    const auto existing = output_cloud.attributes.find(point_scale_attribute);
    if (existing == output_cloud.attributes.end()) {
      const auto base =
          default_point_scale_millimetres().load(std::memory_order_relaxed) *
          point_scale.value;
      auto point_scales = output_cloud.add<float>(point_scale_attribute);
      thrust::fill(thrust::tbb::par, point_scales.begin(), point_scales.end(),
                   base);
    } else if (auto *point_scales =
                   std::get_if<std::vector<float>>(&existing->second.storage)) {
      thrust::transform(
          thrust::tbb::par, point_scales->begin(), point_scales->end(),
          point_scales->begin(),
          [gain = point_scale.value](float value) { return value * gain; });
    } else {
      // if the point_scale is quantised, we can apply any gain directly to its
      // encoding to save having to do an additional transform
      existing->second.encoding.step *= point_scale.value;
      existing->second.encoding.offset *= point_scale.value;
    }
  }
}

} // namespace pc::backend::cuda