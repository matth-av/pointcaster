#include "draco/attributes/point_attribute.h"
#include "draco/metadata/geometry_metadata.h"
#include <draco/compression/decode.h>
#include <draco/compression/draco_compression_options.h>
#include <draco/compression/encode.h>
#include <draco/point_cloud/point_cloud_builder.h>
#include <memory>
#include <pointcaster/point_cloud.h>
#include <string>
#include <type_traits>

namespace pc {

using namespace draco;

namespace {
template <typename T> constexpr DataType draco_data_type() {
  if constexpr (std::is_same_v<T, float>) {
    return DataType::DT_FLOAT32;
  } else if constexpr (sizeof(typename T::storage_type) == 1) {
    return DataType::DT_UINT8;
  } else if constexpr (sizeof(typename T::storage_type) == 2) {
    return DataType::DT_UINT16;
  } else {
    return DataType::DT_UINT32;
  }
}
} // namespace

std::vector<std::byte> PointCloud::compress() const {
  PointCloudBuilder draco_builder;
  draco_builder.Start(size());
  auto pos_attribute_id = draco_builder.AddAttribute(PointAttribute::POSITION,
                                                     4, DataType::DT_INT16);
  draco_builder.SetAttributeValuesForAllPoints(
      pos_attribute_id, positions.data(), sizeof(position));
  auto col_attribute_id =
      draco_builder.AddAttribute(PointAttribute::COLOR, 4, DataType::DT_UINT8);
  draco_builder.SetAttributeValuesForAllPoints(col_attribute_id, colors.data(),
                                               sizeof(color));

  // custom attributes go in as draco generic attributes.
  for (const auto &[name, storage] : attributes) {
    std::visit(
        [&](const auto &values) {
          if (values.size() != size()) return;
          using element_t = typename std::decay_t<decltype(values)>::value_type;
          // scale is already an unsigned normalised integer, so it goes over
          // as one and draco encodes it losslessly
          auto attribute_id = draco_builder.AddAttribute(
              PointAttribute::GENERIC, 1, draco_data_type<element_t>());
          draco_builder.SetAttributeValuesForAllPoints(
              attribute_id, values.data(), sizeof(element_t));
          Metadata metadata;
          metadata.AddEntryString("name", name);
          draco_builder.AddAttributeMetadata(
              attribute_id, std::make_unique<AttributeMetadata>(metadata));
        },
        storage);
  }

  // Finalize() below takes a bool specifying if we should run a
  // deduplication step. It's generally too slow to use in real-time
  auto draco_point_cloud = draco_builder.Finalize(false);

  // after moving our point cloud into the draco data type, we can now
  // compress it
  Encoder encoder;

  // TODO these need to be read from the config options
  // the following speed option prioritises decoding speed over
  // both compression ratio and encoding speed
  encoder.SetSpeedOptions(-1, 10);

  // This only applies to the float32 generic attributes I think...
  encoder.SetAttributeQuantization(PointAttribute::GENERIC, 11);

  EncoderBuffer out_buffer;
  encoder.EncodePointCloudToBuffer(*draco_point_cloud, &out_buffer);
  auto buffer_ptr = reinterpret_cast<const std::byte *>(out_buffer.data());
  std::vector<std::byte> output_data;
  output_data.assign(buffer_ptr, buffer_ptr + out_buffer.size());
  return output_data;
}

PointCloud PointCloud::decompress(const std::vector<std::byte> &buffer,
                                  unsigned long point_count) {
  Decoder decoder;
  DecoderBuffer in_buffer;
  auto buffer_ptr = reinterpret_cast<const char *>(buffer.data());
  in_buffer.Init(buffer_ptr, buffer.size());

  auto draco_point_cloud =
      decoder.DecodePointCloudFromBuffer(&in_buffer).value();
  auto positions_attr_id =
      draco_point_cloud->GetNamedAttributeId(PointAttribute::POSITION);
  auto colors_attr_id =
      draco_point_cloud->GetNamedAttributeId(PointAttribute::COLOR);
  auto draco_positions =
      draco_point_cloud->GetAttributeByUniqueId(positions_attr_id);
  auto draco_colors = draco_point_cloud->GetAttributeByUniqueId(colors_attr_id);

  // copy point data from the draco type into our PointCloud
  auto input_positions_ptr =
      reinterpret_cast<position *>(draco_positions->buffer()->data());
  auto input_colors_ptr =
      reinterpret_cast<color *>(draco_colors->buffer()->data());

  PointCloud point_cloud;
  point_cloud.positions.assign(input_positions_ptr,
                               input_positions_ptr + point_count);
  point_cloud.colors.assign(input_colors_ptr, input_colors_ptr + point_count);

  // and the generic attributes back out, keyed by the name we stored
  for (int32_t attribute_id = 0;
       attribute_id < draco_point_cloud->num_attributes(); ++attribute_id) {
    auto draco_attribute = draco_point_cloud->attribute(attribute_id);
    if (draco_attribute->attribute_type() != PointAttribute::GENERIC) continue;

    auto metadata =
        draco_point_cloud->GetAttributeMetadataByAttributeId(attribute_id);
    std::string name;
    if (!metadata || !metadata->GetEntryString("name", &name)) continue;

    auto input_bytes = draco_attribute->buffer()->data();
    if (draco_attribute->data_type() == draco_data_type<float>()) {
      auto input_values_ptr = reinterpret_cast<float *>(input_bytes);
      point_cloud.attributes.insert_or_assign(
          name, std::vector<float>(input_values_ptr,
                                   input_values_ptr + point_count));
    } else if (draco_attribute->data_type() == draco_data_type<scale>()) {
      auto input_values_ptr = reinterpret_cast<scale *>(input_bytes);
      point_cloud.attributes.insert_or_assign(
          name, std::vector<scale>(input_values_ptr,
                                   input_values_ptr + point_count));
    }
  }

  return point_cloud;
}
} // namespace pc