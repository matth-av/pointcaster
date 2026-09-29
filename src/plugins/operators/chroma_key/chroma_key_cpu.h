#pragma once

#include "chroma_key_filter.h"

#include <pointcaster/point_cloud.h>

namespace pc::operators::chroma_key {

void key_cloud_cpu(const PointCloud &input_cloud, PointCloud &output_cloud,
                   const KeyParameters &key);

} // namespace pc::operators::chroma_key
