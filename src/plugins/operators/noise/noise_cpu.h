#pragma once

#include "noise_field.h"

#include <pointcaster/point_cloud.h>

namespace pc::operators::noise {

void displace_cloud_cpu(const PointCloud &input_cloud, PointCloud &output_cloud,
                        const DisplacementParameters &displacement);

} // namespace pc::operators::noise
