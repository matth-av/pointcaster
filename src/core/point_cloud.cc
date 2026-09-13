#include "codec/codecs.h"

#include <chrono>
#include <pointcaster/point_cloud.h>
#include <profiling/profiling_zone.h>
#include <stdexcept>
#include <zpp_bits.h>

namespace pc {

using namespace std::chrono;
using namespace pc::profiling;

using PointCloudCodec = CodecConfiguration::PointCloudCodec;

namespace {

// timestamp, point count and codec id are written ahead of the body
constexpr size_t packet_header_bytes =
    sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t);

// what the reflected body will take, so the uncompressed path reserves once
size_t attribute_bytes(const PointCloud &cloud) {
  size_t total = 0;
  for (const auto &[_, storage] : cloud.attributes) {
    total += std::visit(
        [](const auto &values) {
          using ElementT = typename std::decay_t<decltype(values)>::value_type;
          return values.size() * sizeof(ElementT);
        },
        storage);
  }
  return total;
}

} // namespace

auto PointCloud::compress(const CodecConfiguration &codec_config) const
    -> std::vector<std::byte> {
  ProfilingZone zone("PointCloud::compress");
  const auto &compression = codec_config.compression.value().variant();

  const auto timestamp = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
  const uint64_t point_count = size();

  const auto selected =
      empty() ? PointCloudCodec::None : codec_config.codec.value();
  const auto codec_id = static_cast<uint8_t>(selected);
  std::vector<std::byte> buffer;

  if (selected == PointCloudCodec::None) {
    buffer.reserve(packet_header_bytes + positions.size() * sizeof(position) +
                   colors.size() * sizeof(color) + attribute_bytes(*this));
    zpp::bits::out serializer{buffer};
    serializer(timestamp, point_count, codec_id).or_throw();
    serializer(*this).or_throw();
    return buffer;
  }

  std::vector<std::byte> payload;
  {
    ProfilingZone codec_zone("PointCloud::compress::codec");
    if (selected == PointCloudCodec::Meshopt) {
      const auto *options = rfl::get_if<codec::MeshoptOptions>(&compression);
      payload = codec::encode_meshopt(*this, options ? *options
                                                     : codec::MeshoptOptions{});
    } else {
      const auto *options = rfl::get_if<codec::DracoOptions>(&compression);
      payload = codec::encode_draco(*this,
                                    options ? *options : codec::DracoOptions{});
    }
  }

  buffer.reserve(packet_header_bytes + payload.size());
  zpp::bits::out serializer{buffer};
  serializer(timestamp, point_count, codec_id, payload).or_throw();
  return buffer;
}

auto PointCloud::decompress(std::span<const std::byte> buffer) -> PointCloud {
  zpp::bits::in deserializer{buffer};

  uint64_t timestamp = 0;
  uint64_t point_count = 0;
  uint8_t codec_id = 0;
  deserializer(timestamp, point_count, codec_id).or_throw();

  if (codec_id == static_cast<uint8_t>(PointCloudCodec::None)) {
    PointCloud point_cloud;
    deserializer(point_cloud).or_throw();
    return point_cloud;
  }

  std::vector<std::byte> payload;
  deserializer(payload).or_throw();

  switch (static_cast<PointCloudCodec>(codec_id)) {
  case PointCloudCodec::Meshopt:
    return codec::decode_meshopt(payload, point_count);
  case PointCloudCodec::Draco:
    return codec::decode_draco(payload, point_count);
  default:
    throw std::runtime_error("point cloud packet uses an unknown codec");
  }
}

void PointCloud::gather_attributes_into(
    PointCloud &destination, std::span<const uint32_t> indices) const {
  if (attributes.empty()) {
    destination.attributes.clear();
    return;
  }

  StringMap<attribute_storage> gathered;
  gathered.reserve(attributes.size());

  for (const auto &[name, storage] : attributes) {
    std::visit(
        [&](const auto &values) {
          using ElementT = typename std::decay_t<decltype(values)>::value_type;
          std::vector<ElementT> kept(indices.size());
          for (size_t i = 0; i < indices.size(); i++) {
            const auto source_index = static_cast<size_t>(indices[i]);
            if (source_index < values.size()) kept[i] = values[source_index];
          }
          gathered.emplace(name, std::move(kept));
        },
        storage);
  }

  destination.attributes = std::move(gathered);
}

PointCloud &operator+=(PointCloud &lhs, PointCloud const &rhs) {
  const auto appended_size = rhs.size();
  if (appended_size == 0) return lhs;

  const auto original_size = lhs.size();

  lhs.positions.reserve(original_size + appended_size);
  lhs.positions.insert(lhs.positions.end(), rhs.positions.begin(),
                       rhs.positions.end());

  lhs.colors.reserve(original_size + appended_size);
  lhs.colors.insert(lhs.colors.end(), rhs.colors.begin(), rhs.colors.end());

  for (const auto &appended_position : rhs.positions) {
    lhs.bounds.encompass(appended_position);
  }

  // grow what the left cloud already holds, taking values from the right
  // where it carries the same attribute at the same type
  for (auto &[name, storage] : lhs.attributes) {
    std::visit(
        [&](auto &values) {
          using element = typename std::decay_t<decltype(values)>::value_type;
          values.resize(original_size);

          const auto it = rhs.attributes.find(name);
          const auto *appended =
              it == rhs.attributes.end()
                  ? nullptr
                  : std::get_if<std::vector<element>>(&it->second);

          if (appended && appended->size() == appended_size) {
            values.insert(values.end(), appended->begin(), appended->end());
          } else {
            values.resize(original_size + appended_size);
          }
        },
        storage);
  }

  // then take on the attributes only the right cloud has, defaulted across
  // the points that were already here
  for (const auto &[name, storage] : rhs.attributes) {
    if (lhs.attributes.contains(name)) continue;
    std::visit(
        [&](const auto &values) {
          using element = typename std::decay_t<decltype(values)>::value_type;
          std::vector<element> merged(original_size);
          merged.insert(merged.end(), values.begin(), values.end());
          merged.resize(original_size + appended_size);
          lhs.attributes.emplace(name, std::move(merged));
        },
        storage);
  }

  return lhs;
}

PointCloud operator+(PointCloud const &lhs, PointCloud const &rhs) {
  PointCloud combined = lhs;
  combined += rhs;
  return combined;
}

} // namespace pc