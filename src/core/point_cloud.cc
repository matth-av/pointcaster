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

// TODO maybe the packet header size should be taken deterministically at
// compile time, maybe even with a struct, not just remembered here with a
// comment...
// timestamp, point count and codec id are written ahead of the body
constexpr size_t packet_header_bytes =
    sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t);

// what the reflected body will take, so the uncompressed path reserves once
size_t attribute_bytes(const PointCloud &cloud) {
  size_t total = 0;
  for (const auto &[_, attribute] : cloud.attributes) {
    total += std::visit(
        [](const auto &values) {
          using ElementT = typename std::decay_t<decltype(values)>::value_type;
          return values.size() * sizeof(ElementT);
        },
        attribute.storage);
  }
  return total;
}

bool holds_scalar(const PointCloud::Attribute &attribute) {
  return std::visit(
      [](const auto &values) {
        using element = typename std::decay_t<decltype(values)>::value_type;
        return std::is_arithmetic_v<element>;
      },
      attribute.storage);
}

// a scalar attribute's elements read back through its encoding
std::vector<float> read_scalar_values(const PointCloud::Attribute &attribute) {
  return std::visit(
      [&](const auto &values) {
        using element = typename std::decay_t<decltype(values)>::value_type;
        std::vector<float> real_values;
        if constexpr (std::is_arithmetic_v<element>) {
          real_values.reserve(values.size());
          for (const auto &value : values) {
            real_values.push_back(
                attribute.encoding.to_value(static_cast<float>(value)));
          }
        }
        return real_values;
      },
      attribute.storage);
}

// the default value a point takes for an attribute its own cloud didn't already
// carry... this is usually just the default constructor {} but point_scale
// needs to be specialised
template <typename T>
T default_attribute_value(std::string_view name,
                          const AttributeEncoding &encoding) {
  if constexpr (std::is_same_v<T, float>) {
    if (name == point_scale_attribute) {
      return default_point_scale_millimetres().load(std::memory_order_relaxed);
    }
  } else if constexpr (std::is_integral_v<T>) {
    if (name == point_scale_attribute) {
      return quantise<T>(
          default_point_scale_millimetres().load(std::memory_order_relaxed),
          encoding);
    }
    // an offset encoding does not put a real zero at a raw zero
    return quantise<T>(0.0f, encoding);
  }
  return T{};
}

} // namespace

auto PointCloud::compress(const CodecConfiguration &codec_config) const
    -> std::vector<std::byte> {
  ProfilingZone zone("PointCloud::compress");
  const auto &compression = codec_config.compression.variant();

  const auto timestamp = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
  const uint64_t point_count = size();

  const auto selected = empty() ? PointCloudCodec::None : codec_config.codec;
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

std::vector<float>
PointCloud::attribute_values_as_float(std::string_view name) const {
  const auto it = attributes.find(name);
  if (it == attributes.end()) return {};
  return read_scalar_values(it->second);
}

void PointCloud::gather_attributes_into(
    PointCloud &destination, std::span<const uint32_t> indices) const {
  if (attributes.empty()) {
    destination.attributes.clear();
    return;
  }

  StringMap<Attribute> gathered;
  gathered.reserve(attributes.size());

  for (const auto &[name, attribute] : attributes) {
    std::visit(
        [&](const auto &values) {
          using ElementT = typename std::decay_t<decltype(values)>::value_type;
          std::vector<ElementT> kept(indices.size());
          for (size_t i = 0; i < indices.size(); i++) {
            const auto source_index = static_cast<size_t>(indices[i]);
            if (source_index < values.size()) kept[i] = values[source_index];
          }
          gathered.emplace(name,
                           Attribute{std::move(kept), attribute.encoding});
        },
        attribute.storage);
  }

  destination.attributes = std::move(gathered);
}

std::atomic<float> &default_point_scale_millimetres() {
  static std::atomic<float> millimetres{2.5f};
  return millimetres;
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
  // where it carries the same attribute in the same storage and encoding
  for (auto &[name, attribute] : lhs.attributes) {
    const auto right = rhs.attributes.find(name);
    const bool right_holds_any = right != rhs.attributes.end();

    // the same storage read the same way appends without touching a value
    const bool right_holds_same =
        right_holds_any &&
        right->second.storage.index() == attribute.storage.index() &&
        right->second.encoding == attribute.encoding;

    // two different storages or steps are both just promoted to floats atm.
    // TODO how else would they consolidate into a single cloud?
    // maybe all that's needed is to indicate to the user in the UI somewhere
    // that this widening of data is occuring
    const bool promote_to_float = right_holds_any && !right_holds_same &&
                                  holds_scalar(attribute) &&
                                  holds_scalar(right->second);

    if (promote_to_float) {
      const auto missing = default_attribute_value<float>(name, {});
      auto merged = read_scalar_values(attribute);
      merged.resize(original_size, missing);
      const auto appended = read_scalar_values(right->second);
      merged.insert(merged.end(), appended.begin(), appended.end());
      merged.resize(original_size + appended_size, missing);

      attribute.storage = std::move(merged);
      attribute.encoding = {};
      continue;
    }

    std::visit(
        [&](auto &values) {
          using element = typename std::decay_t<decltype(values)>::value_type;
          values.resize(original_size);

          const auto *appended =
              right_holds_same
                  ? std::get_if<std::vector<element>>(&right->second.storage)
                  : nullptr;

          if (appended && appended->size() == appended_size) {
            values.insert(values.end(), appended->begin(), appended->end());
          } else {
            values.resize(
                original_size + appended_size,
                default_attribute_value<element>(name, attribute.encoding));
          }
        },
        attribute.storage);
  }

  // then take on the attributes only the right cloud has, defaulted across
  // the points that were already here
  for (const auto &[name, attribute] : rhs.attributes) {
    if (lhs.attributes.contains(name)) continue;
    std::visit(
        [&](const auto &values) {
          using element = typename std::decay_t<decltype(values)>::value_type;
          const auto missing =
              default_attribute_value<element>(name, attribute.encoding);
          std::vector<element> merged(original_size, missing);
          merged.insert(merged.end(), values.begin(), values.end());
          merged.resize(original_size + appended_size, missing);
          lhs.attributes.emplace(
              name,
              PointCloud::Attribute{std::move(merged), attribute.encoding});
        },
        attribute.storage);
  }

  return lhs;
}

PointCloud operator+(PointCloud const &lhs, PointCloud const &rhs) {
  PointCloud combined = lhs;
  combined += rhs;
  return combined;
}

} // namespace pc