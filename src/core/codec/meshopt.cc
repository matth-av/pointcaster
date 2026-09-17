#include "codecs.h"

#include <cstring>
#include <meshoptimizer.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <zpp_bits.h>

namespace pc::codec {

namespace {

struct StreamShape {
  size_t vertex_size;
  size_t vertex_count;
};

StreamShape stream_shape(size_t element_count, size_t element_size) {
  const size_t total_bytes = element_count * element_size;
  const size_t vertex_size = element_size % 4 == 0 ? element_size : 4;
  return {vertex_size, (total_bytes + vertex_size - 1) / vertex_size};
}

std::vector<std::byte> encode_stream(const void *data, size_t element_count,
                                     size_t element_size, int level) {
  const auto [vertex_size, vertex_count] =
      stream_shape(element_count, element_size);
  const size_t total_bytes = element_count * element_size;
  const size_t padded_bytes = vertex_count * vertex_size;

  std::vector<std::byte> padded;
  const void *source = data;
  if (padded_bytes != total_bytes) {
    padded.assign(padded_bytes, std::byte{0});
    std::memcpy(padded.data(), data, total_bytes);
    source = padded.data();
  }

  std::vector<std::byte> encoded(
      meshopt_encodeVertexBufferBound(vertex_count, vertex_size));
  const size_t written = meshopt_encodeVertexBufferLevel(
      reinterpret_cast<unsigned char *>(encoded.data()), encoded.size(), source,
      vertex_count, vertex_size, level, -1);
  encoded.resize(written);
  return encoded;
}

bool decode_stream(void *destination, size_t element_count, size_t element_size,
                   const std::vector<std::byte> &encoded) {
  const auto [vertex_size, vertex_count] =
      stream_shape(element_count, element_size);
  const size_t total_bytes = element_count * element_size;
  const size_t padded_bytes = vertex_count * vertex_size;
  const auto *source = reinterpret_cast<const unsigned char *>(encoded.data());

  if (padded_bytes == total_bytes) {
    return meshopt_decodeVertexBuffer(destination, vertex_count, vertex_size,
                                      source, encoded.size()) == 0;
  }
  std::vector<std::byte> padded(padded_bytes);
  const int result = meshopt_decodeVertexBuffer(
      padded.data(), vertex_count, vertex_size, source, encoded.size());
  std::memcpy(destination, padded.data(), total_bytes);
  return result == 0;
}

struct AttributeStream {
  std::string name;
  uint8_t alternative;
  AttributeEncoding encoding;
  std::vector<std::byte> encoded;
};

struct Payload {
  std::vector<std::byte> positions;
  std::vector<std::byte> colors;
  position_bounds bounds;
  std::vector<AttributeStream> attributes;
};

template <size_t Index>
bool rebuild_attribute(PointCloud &cloud, const AttributeStream &attribute) {
  using storage =
      std::variant_alternative_t<Index, PointCloud::attribute_storage>;
  using ElementT = typename storage::value_type;
  storage values(cloud.size());
  if (!decode_stream(values.data(), values.size(), sizeof(ElementT),
                     attribute.encoded)) {
    return false;
  }
  cloud.attributes.insert_or_assign(
      attribute.name,
      PointCloud::Attribute{std::move(values), attribute.encoding});
  return true;
}

// tries the alternative the tag names, so a new alternative in
// attribute_storage needs nothing here
template <size_t... Indices>
bool rebuild_attribute(PointCloud &cloud, const AttributeStream &attribute,
                       std::index_sequence<Indices...>) {
  return ((attribute.alternative == Indices &&
           rebuild_attribute<Indices>(cloud, attribute)) ||
          ...);
}

} // namespace

std::vector<std::byte> encode_meshopt(const PointCloud &cloud,
                                      const MeshoptOptions &options) {
  const int level = options.level;

  Payload payload;
  payload.positions = encode_stream(cloud.positions.data(), cloud.size(),
                                    sizeof(position), level);
  payload.colors =
      encode_stream(cloud.colors.data(), cloud.size(), sizeof(color), level);
  payload.bounds = cloud.bounds;

  for (const auto &[name, attribute] : cloud.attributes) {
    std::visit(
        [&](const auto &values) {
          using element = typename std::decay_t<decltype(values)>::value_type;
          if (values.size() != cloud.size()) return;
          payload.attributes.push_back(
              {name, static_cast<uint8_t>(attribute.storage.index()),
               attribute.encoding,
               encode_stream(values.data(), values.size(), sizeof(element),
                             level)});
        },
        attribute.storage);
  }

  std::vector<std::byte> buffer;
  zpp::bits::out serializer{buffer};
  serializer(payload).or_throw();
  return buffer;
}

PointCloud decode_meshopt(std::span<const std::byte> buffer,
                          size_t point_count) {
  Payload payload;
  zpp::bits::in deserializer{buffer};
  deserializer(payload).or_throw();

  // the decoder is safe on untrusted input but produces garbage rather than
  // failing loudly, so its result decides whether this frame exists at all
  PointCloud point_cloud;
  point_cloud.resize(point_count);
  if (!decode_stream(point_cloud.positions.data(), point_count,
                     sizeof(position), payload.positions) ||
      !decode_stream(point_cloud.colors.data(), point_count, sizeof(color),
                     payload.colors)) {
    throw std::runtime_error("meshopt payload did not decode");
  }
  point_cloud.bounds = payload.bounds;

  constexpr auto alternatives = std::make_index_sequence<
      std::variant_size_v<PointCloud::attribute_storage>>{};
  for (const auto &attribute : payload.attributes) {
    if (!rebuild_attribute(point_cloud, attribute, alternatives)) {
      throw std::runtime_error("meshopt attribute stream did not decode");
    }
  }
  return point_cloud;
}

} // namespace pc::codec
