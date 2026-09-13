#pragma once

#include <pointcaster/point_cloud.h>

namespace pc::codec {

using DracoOptions = CodecConfiguration::DracoCompressionConfiguration;
using MeshoptOptions = CodecConfiguration::MeshoptCompressionConfiguration;

std::vector<std::byte> encode_draco(const PointCloud &cloud,
                                    const DracoOptions &options);
PointCloud decode_draco(std::span<const std::byte> buffer, size_t point_count);

std::vector<std::byte> encode_meshopt(const PointCloud &cloud,
                                      const MeshoptOptions &options);
PointCloud decode_meshopt(std::span<const std::byte> buffer,
                          size_t point_count);

} // namespace pc::codec
