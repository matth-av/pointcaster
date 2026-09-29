#include "chroma_key_cpu.h"

#include <algorithm>
#include <execution>
#include <iterator>
#include <plugins/backend/backend_filters.h>
#include <profiling/profiling_zone.h>
#include <ranges>
#include <vector>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace pc::operators::chroma_key {

void key_cloud_cpu(const PointCloud &input_cloud, PointCloud &output_cloud,
                   const KeyParameters &key) {
  using pc::profiling::ProfilingZone;

  const auto &input_positions = input_cloud.positions;
  const auto &input_colors = input_cloud.colors;
  const auto input_count = input_positions.size();

  std::vector<uint32_t> kept_indices(input_count);
  {
    ProfilingZone select_zone("ChromaKeyOperator::cpu_select");
    auto index_sequence =
        std::views::iota(uint32_t{0}, static_cast<uint32_t>(input_count));
    auto kept_end = std::copy_if(
        std::execution::par_unseq, index_sequence.begin(), index_sequence.end(),
        kept_indices.begin(),
        [&](uint32_t i) {
          return in_key(input_colors[i], key) == key.invert;
        });
    kept_indices.resize(
        static_cast<size_t>(std::distance(kept_indices.begin(), kept_end)));
  }

  ProfilingZone write_zone("ChromaKeyOperator::cpu_write_output");

  const auto kept_count = kept_indices.size();

  input_cloud.gather_attributes_into(output_cloud, kept_indices);
  output_cloud.resize(kept_count);

  output_cloud.bounds = tbb::parallel_reduce(
      tbb::blocked_range<size_t>(0, kept_count), position_bounds{},
      [&](const tbb::blocked_range<size_t> &range, position_bounds running) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
          const auto source_index = kept_indices[i];
          const auto &point = input_positions[source_index];
          output_cloud.positions[i] = point;
          output_cloud.colors[i] = input_colors[source_index];
          running.encompass(point);
        }
        return running;
      },
      backend::filter::merge_bounds);
}

} // namespace pc::operators::chroma_key
