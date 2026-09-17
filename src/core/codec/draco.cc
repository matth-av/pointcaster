#include "codecs.h"
#include "draco/attributes/point_attribute.h"
#include "draco/metadata/geometry_metadata.h"
#include <algorithm>
#include <draco/compression/decode.h>
#include <draco/compression/draco_compression_options.h>
#include <draco/compression/encode.h>
#include <draco/point_cloud/point_cloud_builder.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace pc::codec {

using namespace draco;

namespace {

// the key for the array in a draco frame's metadata that holds an attribute's
// encoding configuration if its precision has been lowered
constexpr const char *attribute_encoding_key = "encoding";

template <typename Scalar> constexpr DataType draco_scalar_type() {
  if constexpr (std::is_same_v<Scalar, float>) {
    return DataType::DT_FLOAT32;
  } else if constexpr (std::is_same_v<Scalar, uint8_t>) {
    return DataType::DT_UINT8;
  } else if constexpr (std::is_same_v<Scalar, int8_t>) {
    return DataType::DT_INT8;
  } else if constexpr (std::is_same_v<Scalar, uint16_t>) {
    return DataType::DT_UINT16;
  } else if constexpr (std::is_same_v<Scalar, uint32_t>) {
    return DataType::DT_UINT32;
  } else if constexpr (std::is_same_v<Scalar, int16_t>) {
    return DataType::DT_INT16;
  } else if constexpr (std::is_same_v<Scalar, int32_t>) {
    return DataType::DT_INT32;
  } else {
    static_assert(false, "attribute scalar has no draco data type");
  }
}

// how one element of an attribute vector is laid out for draco
struct AttributeFormat {
  DataType data_type;
  int8_t component_count;
};

template <typename T> constexpr AttributeFormat draco_attribute_format() {
  if constexpr (std::is_arithmetic_v<T>) {
    return {draco_scalar_type<T>(), 1};
  } else if constexpr (requires(T value) {
                         value.x;
                         value.y;
                       }) {
    using component = decltype(T::x);
    return {draco_scalar_type<component>(),
            static_cast<int8_t>(sizeof(T) / sizeof(component))};
  } else {
    static_assert(false, "attribute element has no draco format");
  }
}

} // namespace

std::vector<std::byte> encode_draco(const PointCloud &cloud,
                                    const DracoOptions &options) {
  PointCloudBuilder draco_builder;
  draco_builder.Start(cloud.size());
  auto pos_attribute_id = draco_builder.AddAttribute(PointAttribute::POSITION,
                                                     4, DataType::DT_INT16);
  draco_builder.SetAttributeValuesForAllPoints(
      pos_attribute_id, cloud.positions.data(), sizeof(position));
  auto col_attribute_id =
      draco_builder.AddAttribute(PointAttribute::COLOR, 4, DataType::DT_UINT8);
  draco_builder.SetAttributeValuesForAllPoints(
      col_attribute_id, cloud.colors.data(), sizeof(color));

  // custom attributes go in as draco generic attributes.
  for (const auto &[name, attribute] : cloud.attributes) {
    std::visit(
        [&](const auto &values) {
          if (values.size() != cloud.size()) return;
          using element_t = typename std::decay_t<decltype(values)>::value_type;
          constexpr auto format = draco_attribute_format<element_t>();
          auto attribute_id = draco_builder.AddAttribute(
              PointAttribute::GENERIC, format.component_count,
              format.data_type);
          draco_builder.SetAttributeValuesForAllPoints(
              attribute_id, values.data(), sizeof(element_t));
          Metadata metadata;
          metadata.AddEntryString("name", name);
          // only a quantised attribute needs to say how to read it back
          if (!(attribute.encoding == AttributeEncoding{})) {
            metadata.AddEntryDoubleArray(
                attribute_encoding_key,
                {attribute.encoding.step, attribute.encoding.offset});
          }
          draco_builder.AddAttributeMetadata(
              attribute_id, std::make_unique<AttributeMetadata>(metadata));
        },
        attribute.storage);
  }

  auto draco_point_cloud = draco_builder.Finalize(false);

  if (!draco_point_cloud->metadata()) {
    draco_point_cloud->AddMetadata(std::make_unique<GeometryMetadata>());
  }
  const auto &bounds = cloud.bounds;
  draco_point_cloud->metadata()->AddEntryIntArray(
      "bounds", {bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x,
                 bounds.max.y, bounds.max.z});

  Encoder encoder;
  encoder.SetSpeedOptions(options.encode_speed, options.decode_speed);

  const auto quantization = options.attribute_quantization;
  if (quantization.active) {
    encoder.SetAttributeQuantization(PointAttribute::GENERIC,
                                     std::clamp(quantization.value, 1, 30));
  }

  EncoderBuffer out_buffer;
  const auto encoded =
      encoder.EncodePointCloudToBuffer(*draco_point_cloud, &out_buffer);
  if (!encoded.ok()) {
    throw std::runtime_error(encoded.error_msg_string());
  }
  auto buffer_ptr = reinterpret_cast<const std::byte *>(out_buffer.data());
  std::vector<std::byte> output_data;
  output_data.assign(buffer_ptr, buffer_ptr + out_buffer.size());
  return output_data;
}

PointCloud decode_draco(std::span<const std::byte> buffer, size_t point_count) {
  Decoder decoder;
  DecoderBuffer in_buffer;
  auto buffer_ptr = reinterpret_cast<const char *>(buffer.data());
  in_buffer.Init(buffer_ptr, buffer.size());

  auto decoded = decoder.DecodePointCloudFromBuffer(&in_buffer);
  if (!decoded.ok()) {
    throw std::runtime_error(decoded.status().error_msg_string());
  }
  auto draco_point_cloud = std::move(decoded).value();

  if (draco_point_cloud->num_points() != point_count) {
    throw std::runtime_error(
        "draco payload point count disagrees with the packet header");
  }

  const auto positions_attr_id =
      draco_point_cloud->GetNamedAttributeId(PointAttribute::POSITION);
  const auto colors_attr_id =
      draco_point_cloud->GetNamedAttributeId(PointAttribute::COLOR);
  if (positions_attr_id < 0 || colors_attr_id < 0) {
    throw std::runtime_error("draco payload is missing positions or colours");
  }
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

    // an attribute encoded without one was already holding real values
    AttributeEncoding encoding;
    std::vector<double> encoded_encoding;
    if (metadata->GetEntryDoubleArray(attribute_encoding_key,
                                      &encoded_encoding) &&
        encoded_encoding.size() == 2) {
      encoding = {static_cast<float>(encoded_encoding[0]),
                  static_cast<float>(encoded_encoding[1])};
    }

    // the data type and component count together say which alternative was
    // encoded, so a new one in attribute_storage adds a line below
    const auto rebuild = [&]<typename T>(std::type_identity<T>) {
      constexpr auto format = draco_attribute_format<T>();
      if (draco_attribute->data_type() != format.data_type ||
          draco_attribute->num_components() != format.component_count) {
        return false;
      }
      const auto *input_values_ptr =
          reinterpret_cast<const T *>(draco_attribute->buffer()->data());
      point_cloud.attributes.insert_or_assign(
          name,
          PointCloud::Attribute{
              std::vector<T>(input_values_ptr, input_values_ptr + point_count),
              encoding});
      return true;
    };

    [[maybe_unused]] const bool rebuilt =
        rebuild(std::type_identity<float>{}) ||
        rebuild(std::type_identity<position>{}) ||
        rebuild(std::type_identity<uint8_t>{}) ||
        rebuild(std::type_identity<uint16_t>{}) ||
        rebuild(std::type_identity<int8_t>{}) ||
        rebuild(std::type_identity<int16_t>{});
  }

  if (const auto *geometry_metadata = draco_point_cloud->GetMetadata()) {
    std::vector<int32_t> encoded_bounds;
    if (geometry_metadata->GetEntryIntArray("bounds", &encoded_bounds) &&
        encoded_bounds.size() == 6) {
      point_cloud.bounds.min = {static_cast<int16_t>(encoded_bounds[0]),
                                static_cast<int16_t>(encoded_bounds[1]),
                                static_cast<int16_t>(encoded_bounds[2])};
      point_cloud.bounds.max = {static_cast<int16_t>(encoded_bounds[3]),
                                static_cast<int16_t>(encoded_bounds[4]),
                                static_cast<int16_t>(encoded_bounds[5])};
    }
  }

  return point_cloud;
}
} // namespace pc::codec