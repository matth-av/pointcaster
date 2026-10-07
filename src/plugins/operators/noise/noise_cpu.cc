#include "noise_cpu.h"

#include <plugins/backend/backend_filters.h>
#include <profiling/profiling_zone.h>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace pc::operators::noise {

void displace_cloud_cpu(const PointCloud &input_cloud, PointCloud &output_cloud,
                        const DisplacementParameters &displacement) {
  using pc::profiling::ProfilingZone;
  ProfilingZone displace_zone("NoiseOperator::cpu_displace");

  output_cloud = input_cloud;

  auto &positions = output_cloud.positions;
  output_cloud.bounds = tbb::parallel_reduce(
      tbb::blocked_range<size_t>(0, positions.size()), position_bounds{},
      [&](const tbb::blocked_range<size_t> &range, position_bounds running) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
          positions[i] = displace(positions[i], displacement);
          running.encompass(positions[i]);
        }
        return running;
      },
      backend::filter::merge_bounds);
}

} // namespace pc::operators::noise
