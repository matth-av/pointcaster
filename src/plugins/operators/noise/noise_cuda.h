#pragma once

#include "noise_field.h"

#include <memory>
#include <pointcaster/point_cloud.h>

namespace pc::operators::noise {

class CudaNoise {
public:
  CudaNoise();
  ~CudaNoise();

  CudaNoise(const CudaNoise &) = delete;
  CudaNoise &operator=(const CudaNoise &) = delete;
  CudaNoise(CudaNoise &&) = delete;
  CudaNoise &operator=(CudaNoise &&) = delete;

  void displace_cloud(const PointCloud &input_cloud, PointCloud &output_cloud,
                      const DisplacementParameters &displacement);

private:
  struct DeviceMemory;
  std::unique_ptr<DeviceMemory> _device_memory;
};

} // namespace pc::operators::noise
