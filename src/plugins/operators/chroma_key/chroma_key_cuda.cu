#include "chroma_key_cuda.h"

#include <plugins/backend/backend_filters.h>
#include <profiling/profiling_zone.h>
#include <thrust/copy.h>
#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/transform_reduce.h>
#include <vector>

namespace pc::operators::chroma_key {

struct CudaChromaKey::DeviceMemory {
  thrust::device_vector<position> input_positions;
  thrust::device_vector<color> input_colors;
  thrust::device_vector<position> output_positions;
  thrust::device_vector<color> output_colors;
  thrust::device_vector<uint32_t> output_indices;

  void ensure_capacity(size_t point_count) {
    if (point_count <= input_positions.size()) return;
    input_positions.resize(point_count);
    input_colors.resize(point_count);
    output_positions.resize(point_count);
    output_colors.resize(point_count);
    output_indices.resize(point_count);
  }
};

CudaChromaKey::CudaChromaKey()
    : _device_memory(std::make_unique<DeviceMemory>()) {}

CudaChromaKey::~CudaChromaKey() = default;

void CudaChromaKey::key_cloud(const PointCloud &input_cloud,
                              PointCloud &output_cloud,
                              const KeyParameters &key) {
  using pc::profiling::ProfilingZone;

  auto &memory = *_device_memory;
  const size_t input_count = input_cloud.size();
  memory.ensure_capacity(input_count);

  {
    ProfilingZone copy_zone("ChromaKeyOperator::cuda_copy_to_device");
    thrust::copy(input_cloud.positions.begin(), input_cloud.positions.end(),
                 memory.input_positions.begin());
    thrust::copy(input_cloud.colors.begin(), input_cloud.colors.end(),
                 memory.input_colors.begin());
  }

  size_t kept_count;
  position_bounds kept_bounds;

  {
    ProfilingZone select_zone("ChromaKeyOperator::cuda_select");

    auto input_points_begin = thrust::make_zip_iterator(
        thrust::make_tuple(memory.input_positions.begin(),
                           memory.input_colors.begin(),
                           thrust::make_counting_iterator(uint32_t{0})));
    auto output_points_begin = thrust::make_zip_iterator(
        thrust::make_tuple(memory.output_positions.begin(),
                           memory.output_colors.begin(),
                           memory.output_indices.begin()));

    auto kept_end = thrust::copy_if(
        thrust::cuda::par, input_points_begin, input_points_begin + input_count,
        output_points_begin,
        [key] __host__ __device__(
            thrust::tuple<position, color, uint32_t> point) {
          return in_key(thrust::get<1>(point), key) == key.invert;
        });
    kept_count = static_cast<size_t>(kept_end - output_points_begin);

    kept_bounds = thrust::transform_reduce(
        thrust::cuda::par, memory.output_positions.begin(),
        memory.output_positions.begin() + kept_count,
        [] __host__ __device__(const position &p) {
          return backend::filter::as_bounds(p);
        },
        position_bounds{},
        [] __host__ __device__(const position_bounds &a,
                               const position_bounds &b) {
          return backend::filter::merge_bounds(a, b);
        });
  }

  ProfilingZone output_zone("ChromaKeyOperator::cuda_copy_to_host");

  // the index list only has to come back off the gpu when there are
  // attributes for it to place
  std::vector<uint32_t> kept_indices;
  if (!input_cloud.attributes.empty()) {
    kept_indices.resize(kept_count);
    thrust::copy(memory.output_indices.begin(),
                 memory.output_indices.begin() + kept_count,
                 kept_indices.begin());
  }

  input_cloud.gather_attributes_into(output_cloud, kept_indices);

  output_cloud.resize(kept_count);
  thrust::copy(memory.output_positions.begin(),
               memory.output_positions.begin() + kept_count,
               output_cloud.positions.begin());
  thrust::copy(memory.output_colors.begin(),
               memory.output_colors.begin() + kept_count,
               output_cloud.colors.begin());
  output_cloud.bounds = kept_bounds;
}

} // namespace pc::operators::chroma_key
