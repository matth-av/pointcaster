#pragma once

#include "chroma_key_filter.h"

#include <memory>
#include <pointcaster/point_cloud.h>

namespace pc::operators::chroma_key {

class CudaChromaKey {
public:
  CudaChromaKey();
  ~CudaChromaKey();

  CudaChromaKey(const CudaChromaKey &) = delete;
  CudaChromaKey &operator=(const CudaChromaKey &) = delete;
  CudaChromaKey(CudaChromaKey &&) = delete;
  CudaChromaKey &operator=(CudaChromaKey &&) = delete;

  void key_cloud(const PointCloud &input_cloud, PointCloud &output_cloud,
                 const KeyParameters &key);

private:
  struct DeviceMemory;
  std::unique_ptr<DeviceMemory> _device_memory;
};

} // namespace pc::operators::chroma_key
