#include "noise_cuda.h"

#include <plugins/backend/backend_filters.h>
#include <profiling/profiling_zone.h>
#include <thrust/copy.h>
#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/transform.h>
#include <thrust/transform_reduce.h>

namespace pc::operators::noise {

struct CudaNoise::DeviceMemory {
  thrust::device_vector<position> positions;

  // the kernel reads its parameters through a pointer, since capturing a
  // struct this size by value has every thread copy it into local memory
  thrust::device_vector<DisplacementParameters> displacement;

  void ensure_capacity(size_t point_count) {
    if (point_count <= positions.size()) return;
    positions.resize(point_count);
  }
};

CudaNoise::CudaNoise() : _device_memory(std::make_unique<DeviceMemory>()) {}

CudaNoise::~CudaNoise() = default;

void CudaNoise::displace_cloud(const PointCloud &input_cloud,
                               PointCloud &output_cloud,
                               const DisplacementParameters &displacement) {
  using pc::profiling::ProfilingZone;

  auto &memory = *_device_memory;
  const size_t point_count = input_cloud.size();
  memory.ensure_capacity(point_count);

  {
    ProfilingZone copy_zone("NoiseOperator::cuda_copy_to_device");
    thrust::copy(input_cloud.positions.begin(), input_cloud.positions.end(),
                 memory.positions.begin());
  }

  const auto positions_begin = memory.positions.begin();
  const auto positions_end = positions_begin + point_count;

  position_bounds displaced_bounds;

  {
    ProfilingZone displace_zone("NoiseOperator::cuda_displace");

    // allocated here rather than up front, so an operator that never runs on
    // the gpu (or a machine without one) never touches the cuda runtime
    memory.displacement.resize(1);
    memory.displacement[0] = displacement;
    const auto *parameters =
        thrust::raw_pointer_cast(memory.displacement.data());

    thrust::transform(thrust::cuda::par, positions_begin, positions_end,
                      positions_begin,
                      [parameters] __host__ __device__(const position &p) {
                        return displace(p, *parameters);
                      });

    displaced_bounds = thrust::transform_reduce(
        thrust::cuda::par, positions_begin, positions_end,
        [] __host__ __device__(const position &p) {
          return backend::filter::as_bounds(p);
        },
        position_bounds{},
        [] __host__ __device__(const position_bounds &a,
                               const position_bounds &b) {
          return backend::filter::merge_bounds(a, b);
        });
  }

  ProfilingZone output_zone("NoiseOperator::cuda_copy_to_host");

  // displacing moves points without adding, dropping or reordering any, so
  // only the positions ever need to visit the gpu
  output_cloud = input_cloud;
  thrust::copy(positions_begin, positions_end, output_cloud.positions.begin());
  output_cloud.bounds = displaced_bounds;
}

} // namespace pc::operators::noise
