#include "cpu_backend.h"

#include "../backend_filters.h"
#include "../backend_utils.h"
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <algorithm>
#include <cmath>
#include <core/logger/logger.h>
#include <execution>
#include <functional>
#include <logger/logger.h>
#include <numeric>
#include <pointcaster/core_types.h>
#include <profiling/profiling_zone.h>
#include <ranges>
#include <vector>

// TODO ensure TBB is linked and loaded
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace pc::backend {

namespace {

// we can accept inputs from different source frame types by having a transform
// function where the following PointSource concept is an argument. It's used as
// a way to convert any data type into the required core type (position&color)
// inside the parallelised hot path
template <typename F>
concept PointSource =
    std::invocable<F, int> &&
    std::same_as<std::invoke_result_t<F, int>, std::pair<position, color>>;

template <PointSource F>
void transform_from(F &&get_point, size_t point_count, PointCloud &output_cloud,
                    const PointCloud &attribute_source,
                    const TransformConfiguration &transform,
                    const ColorTransformConfiguration &color_transform,
                    const pc::float4x4 world_transform) {

  // TODO a few of these per-transform allocations could move to cpu_backend
  // class members and be resized in init

  const auto transform_parameters =
      filter::TransformFilterParameters::from_config(transform,
                                                     color_transform);

  const auto sample_cloud = transform_parameters.sample > 1;

  auto index_sequence = std::views::iota(0, static_cast<int>(point_count));

  const auto transform_point = [&](const auto i) {
    if (sample_cloud && !filter::sample(i, transform_parameters)) {
      output_cloud.positions[i] = filter::invalid_position_value;
      return;
    }
    auto [position, color] = get_point(i);
    output_cloud.positions[i] =
        filter::transform(position, transform_parameters);
    output_cloud.colors[i] =
        filter::color_transform(color, transform_parameters);
  };

  std::for_each(std::execution::par_unseq, index_sequence.begin(),
                index_sequence.end(), transform_point);

  std::vector<uint32_t> output_indices(point_count);
  std::iota(output_indices.begin(), output_indices.end(), uint32_t{0});

  const auto crop_point_to_bounds = [&](const auto i) {
    return filter::is_valid(output_cloud.positions[i]) &&
           filter::in_bounds(output_cloud.positions[i], transform_parameters);
  };

  auto new_end =
      std::partition(std::execution::par_unseq, output_indices.begin(),
                     output_indices.end(), crop_point_to_bounds);

  const size_t new_point_count = std::distance(output_indices.begin(), new_end);

  std::vector<position> output_positions(new_point_count);
  std::vector<color> output_colors(new_point_count);

  auto output_range = std::views::iota(size_t{0}, new_point_count);

  // ancestor group placement, applied after the local transform and crop so
  // cropping stays in the device's own space.
  const bool has_world_transform = !(world_transform == pc::float4x4{});
  const auto place = [&](position p) -> position {
    if (!has_world_transform) return p;
    const float x = float(p.x), y = float(p.y), z = float(p.z);
    const auto &world = world_transform.values;
    return position{
        static_cast<int16_t>(
            std::lround(world[0] * x + world[1] * y + world[2] * z + world[3])),
        static_cast<int16_t>(
            std::lround(world[4] * x + world[5] * y + world[6] * z + world[7])),
        static_cast<int16_t>(std::lround(world[8] * x + world[9] * y +
                                         world[10] * z + world[11])),
    };
  };

  const auto copy_to_output_buffers = [&](const auto i) {
    const auto output_index = output_indices[i];
    output_positions[i] = place(output_cloud.positions[output_index]);
    output_colors[i] = output_cloud.colors[output_index];
  };

  std::for_each(std::execution::par_unseq, output_range.begin(),
                output_range.end(), copy_to_output_buffers);

  output_cloud.bounds =
      std::transform_reduce(std::execution::par_unseq, output_positions.begin(),
                            output_positions.end(), position_bounds{},
                            filter::merge_bounds, filter::as_bounds);

  std::copy(output_positions.begin(), output_positions.end(),
            output_cloud.positions.begin());
  std::copy(output_colors.begin(), output_colors.end(),
            output_cloud.colors.begin());

  attribute_source.gather_attributes_into(
      output_cloud, {output_indices.data(), new_point_count});

  output_cloud.resize(new_point_count);
}

} // namespace

CpuBackend::CpuBackend(Corrade::PluginManager::AbstractManager &manager,
                       Corrade::Containers::StringView plugin)
    : BackendPlugin(manager, plugin) {
  pc::logger()->trace("Initialised CPU backend");
};

CpuBackend::~CpuBackend() {
  pc::logger()->trace("Destroyed CPU backend");
};

void CpuBackend::transform_point_cloud(
    const PointCloud &input_cloud, PointCloud &output_cloud,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform,
    const pc::float4x4 &world_transform) const {

  transform_from(
      [&](int i) -> std::pair<position, color> {
        return {input_cloud.positions[i], input_cloud.colors[i]};
      },
      output_cloud.size(), output_cloud, input_cloud, transform,
      color_transform, world_transform);
}

BoundsFilterResult CpuBackend::filter_to_bounds(
    const PointCloud &input_cloud, PointCloud &output_cloud,
    const position_bounds &bounds, BoundsFilterOptions options) const {
  using namespace pc::profiling;
  ProfilingZone filter_zone("CpuBackend::filter_to_bounds");

  const auto &input_positions = input_cloud.positions;
  const auto &input_colors = input_cloud.colors;
  const auto input_count = input_positions.size();

  BoundsFilterResult result{.input_count = input_count};
  if (input_count == 0) return result;

  const bool invert = options.invert;
  const auto filter_point = [&bounds, invert](const position &p) {
    return filter::in_bounds(p, bounds) != invert;
  };

  // analysing on its own never gathers anything, so it has no use for the
  // index list a write needs: one reduction counts what falls in the box and
  // measures it on the way past
  if (options.analyse_only) {
    ProfilingZone measure_zone("CpuBackend::measure_in_bounds");
    const auto measured = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, input_count), filter::empty_measurement(),
        [&](const tbb::blocked_range<size_t> &range,
            filter::bounds_measurement running) {
          for (size_t i = range.begin(); i != range.end(); ++i) {
            const auto &point = input_positions[i];
            if (!filter_point(point)) continue;
            running.point_count++;
            running.bounds =
                filter::merge_bounds(running.bounds, filter::as_bounds(point));
          }
          return running;
        },
        filter::merge_measurements);

    result.point_count = measured.point_count;
    result.bounds = measured.bounds;
    return result;
  }

  // the kept points are gathered as indices first, so that only one pass runs
  // over the whole input however many of them the box ends up holding
  std::vector<uint32_t> kept_indices(input_count);
  {
    ProfilingZone select_zone("CpuBackend::select");
    auto index_sequence =
        std::views::iota(uint32_t{0}, static_cast<uint32_t>(input_count));
    auto kept_end = std::copy_if(
        std::execution::par_unseq, index_sequence.begin(), index_sequence.end(),
        kept_indices.begin(),
        [&](uint32_t i) { return filter_point(input_positions[i]); });
    kept_indices.resize(
        static_cast<size_t>(std::distance(kept_indices.begin(), kept_end)));
  }

  result.point_count = kept_indices.size();
  if (kept_indices.empty()) return result;

  {
    ProfilingZone write_zone("CpuBackend::write_output");

    const bool writing_in_place = &output_cloud == &input_cloud;
    const std::vector<position> source_positions =
        writing_in_place ? input_positions : std::vector<position>{};
    const std::vector<color> source_colors =
        writing_in_place ? input_colors : std::vector<color>{};
    const auto &read_positions =
        writing_in_place ? source_positions : input_positions;
    const auto &read_colors = writing_in_place ? source_colors : input_colors;

    input_cloud.gather_attributes_into(output_cloud, kept_indices);

    output_cloud.resize(result.point_count);

    result.bounds = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, result.point_count),
        filter::empty_bounds(),
        [&](const tbb::blocked_range<size_t> &range, position_bounds running) {
          for (size_t i = range.begin(); i != range.end(); ++i) {
            const auto source_index = kept_indices[i];
            const auto &point = read_positions[source_index];
            output_cloud.positions[i] = point;
            output_cloud.colors[i] = read_colors[source_index];
            running = filter::merge_bounds(running, filter::as_bounds(point));
          }
          return running;
        },
        filter::merge_bounds);

    output_cloud.bounds = result.bounds;
  }

  return result;
}

void CpuBackend::project_transform_frame_data(
    std::span<const uint16_t> input_depth_frame,
    std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
    const CameraIntrinsics &color_intrinsics,
    const TransformConfiguration &transform,
    const ColorTransformConfiguration &color_transform,
    const pc::float4x4 &world_transform,
    [[maybe_unused]] std::span<std::byte> render_output) const {

  const auto point_count = output_cloud.size();
  const auto frame_width = color_intrinsics.frame_width;

  const auto convert_point_data = [&](int i) -> std::pair<position, color> {
    const auto px = i % frame_width;
    const auto py = i / frame_width;
    const auto pos =
        util::project_2d_to_3d(px, py, input_depth_frame[i], color_intrinsics);
    const auto &rgb = input_rgb_frame[i];
    return {pos, color{rgb.r, rgb.g, rgb.b}};
  };

  transform_from(convert_point_data, point_count, output_cloud, output_cloud,
                 transform, color_transform, world_transform);
}

void CpuBackend::pack_render_buffer(const PointCloud &cloud,
                                    std::span<std::byte> output) const {
  if (output.empty()) return;

  const auto count = cloud.size();
  const auto stride = render_vertex_stride(cloud);
  const auto point_scales = cloud.get<float>(point_scale_attribute);
  const auto indices = std::views::iota(size_t{0}, count);
  auto *out_bytes = reinterpret_cast<char *>(output.data());

  std::for_each(
      std::execution::par_unseq, indices.begin(), indices.end(), [&](size_t i) {
        std::memcpy(out_bytes + i * stride, &cloud.positions[i], 8);
        std::memcpy(out_bytes + i * stride + 8, &cloud.colors[i], 4);
        float idx = static_cast<float>(i);
        std::memcpy(out_bytes + i * stride + 12, &idx, 4);
        if (i < point_scales.size()) {
          std::memcpy(out_bytes + i * stride + 16, &point_scales[i], 4);
        }
      });
};

void CpuBackend::project_frame(const PointCloud &cloud,
                               camera::CameraFrameData &output,
                               camera::FrameProjectionArgs projection) const {

  const auto &[fx, fy, cx, cy, width, height, extrinsic] = projection;
  auto &ext = extrinsic.values;

  auto &color_buffer = output.main_color_buffer();
  auto &depth_buffer = output.depth_buffer();
  auto &index_buffer = output.index_buffer();
  auto &pixel_hits = output.pixel_hits;

  for (size_t point_index = 0; point_index < cloud.size(); point_index++) {

    const auto &pos = cloud.positions[point_index];
    auto wx = static_cast<float>(pos.x);
    auto wy = static_cast<float>(pos.y);
    auto wz = static_cast<float>(pos.z);

    // transform to camera space (row-major multiply)
    float cam_x = ext[0] * wx + ext[1] * wy + ext[2] * wz + ext[3];
    float cam_y = ext[4] * wx + ext[5] * wy + ext[6] * wz + ext[7];
    float cam_z = ext[8] * wx + ext[9] * wy + ext[10] * wz + ext[11];

    // near plane cutoff
    if (cam_z <= 0.0f) continue;

    float u_f = fx * (cam_x / cam_z) + cx;
    float v_f = fy * (cam_y / cam_z) + cy;
    int u = static_cast<int>(std::round(u_f));
    int v = static_cast<int>(std::round(v_f));

    if (u < 0 || u >= width || v < 0 || v >= height) continue;

    int pixel_index = v * width + u;

    // store every point that hits this pixel
    pixel_hits[pixel_index].push_back(
        {static_cast<int32_t>(point_index), cam_z});

    // if this point's z is nearest to the camera, it wins for rendering it
    if (cam_z < depth_buffer[pixel_index]) {
      depth_buffer[pixel_index] = cam_z;
      index_buffer[pixel_index] = point_index;
      auto col = cloud.colors[point_index];
      col.a = 255;
      color_buffer[pixel_index] = col;
    }
  }
}

} // namespace pc::backend

CORRADE_PLUGIN_REGISTER(CpuBackend, pc::backend::CpuBackend,
                        "net.pointcaster.BackendPlugin/1.0")