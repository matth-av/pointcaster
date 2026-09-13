#include <chrono>
#include <iostream>
#include <pointcaster/point_cloud.h>
#include <profiling/profiling_zone.h>
#include <zpp_bits.h>

namespace pc {

using namespace std::chrono;
using namespace pc::profiling;

auto PointCloud::serialize(bool compress) const -> std::vector<std::byte> {
  ProfilingZone zone("PointCloud::serialize");

  const auto timestamp = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
  const uint64_t point_count = size();
  const auto compression_flag = static_cast<uint8_t>(compress);
  std::vector<std::byte> buffer;

  if (compress) {
    std::vector<std::byte> payload;
    {
      ProfilingZone z("serialize::compress");
      payload = this->compress();
    }
    {
      ProfilingZone z("serialize::reserve");
      buffer.reserve(PointCloudPacket::header_bytes + payload.size());
    }
    {
      ProfilingZone z("serialize::write_payload");
      zpp::bits::out serializer{buffer};
      serializer(timestamp, point_count, compression_flag, payload).or_throw();
    }
  } else {
    {
      ProfilingZone z("serialize::reserve");
      buffer.reserve(PointCloudPacket::header_bytes +
                     positions.size() * sizeof(positions[0]) +
                     colors.size() * sizeof(colors[0]));
    }
    zpp::bits::out serializer{buffer};
    {
      ProfilingZone z("serialize::write_header");
      serializer(timestamp, point_count, compression_flag).or_throw();
    }
    {
      ProfilingZone z("serialize::write_body");
      serializer(*this).or_throw();
    }
  }
  return buffer;
}

auto PointCloud::deserialize(std::span<const std::byte> buffer) -> PointCloud {
  zpp::bits::in zpp_deserialize{buffer};

  uint64_t timestamp = 0;
  uint64_t point_count = 0;
  uint8_t compression_flag = 0;
  zpp_deserialize(timestamp, point_count, compression_flag).or_throw();

  if (compression_flag != 0) {
    std::vector<std::byte> payload;
    zpp_deserialize(payload).or_throw();
    return PointCloud::decompress(payload, point_count);
  }

  PointCloud point_cloud;
  zpp_deserialize(point_cloud).or_throw();
  return point_cloud;
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