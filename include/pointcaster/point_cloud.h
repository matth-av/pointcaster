#pragma once

#include "core_types.h"
#include <atomic>
#include <codec/codec_config.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <pointcaster/core.h>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <util/string_map.h>
#include <variant>
#include <vector>

namespace pc {

// a per-point radius, held in the same millimetre space as positions
inline constexpr std::string_view point_scale_attribute = "point_scale";

// the radius a point draws at when the cloud doesn't have its own point_scale
// attribute. based on the point size set in the app preferences window
POINTCASTER_CORE_EXPORT std::atomic<float> &default_point_scale_millimetres();

class PointCloud {
public:
  std::vector<position> positions;
  std::vector<color> colors;

  position_bounds bounds;

  using attribute_storage =
      std::variant<std::vector<position>, std::vector<float>,
                   std::vector<uint8_t>, std::vector<uint16_t>,
                   std::vector<int8_t>, std::vector<int16_t>>;

  // An integer storage holds raw elements that mean what the encoding says
  // they mean. A float storage reads back unchanged through the default.
  struct Attribute {
    attribute_storage storage;
    AttributeEncoding encoding;
  };

  // Arbitrary per-point attributes, keyed by name.
  StringMap<Attribute> attributes;

  auto size() const { return positions.size(); }
  auto empty() const { return positions.empty(); }

  void resize(std::size_t new_size) {
    positions.resize(new_size);
    colors.resize(new_size);
    for (auto &[_, attribute] : attributes) {
      std::visit([new_size](auto &values) { values.resize(new_size); },
                 attribute.storage);
    }
  }

  void reserve(std::size_t new_capacity) {
    positions.reserve(new_capacity);
    colors.reserve(new_capacity);
    for (auto &[_, attribute] : attributes) {
      std::visit([new_capacity](auto &values) { values.reserve(new_capacity); },
                 attribute.storage);
    }
  }

  POINTCASTER_CORE_EXPORT std::vector<std::byte>
  compress(const CodecConfiguration &codec_config = {}) const;

  POINTCASTER_CORE_EXPORT static PointCloud
  decompress(std::span<const std::byte> buffer);

  // Return an attribute by name
  template <typename T>
  [[nodiscard]] std::span<T> get(std::string_view name) noexcept {
    auto it = attributes.find(name);
    if (it == attributes.end()) return {};
    auto *values = std::get_if<std::vector<T>>(&it->second.storage);
    return values ? std::span<T>{*values} : std::span<T>{};
  }

  template <typename T>
  [[nodiscard]] std::span<const T> get(std::string_view name) const noexcept {
    return const_cast<PointCloud *>(this)->get<T>(name);
  }

  // A scalar attribute's real values, whatever it is stored as. get<T>() stays
  // the raw accessor. Empty for an attribute that isn't a scalar.
  [[nodiscard]] POINTCASTER_CORE_EXPORT std::vector<float>
  attribute_values_as_float(std::string_view name) const;

  // Creates or replaces an attribute buffer, reading back unencoded until an
  // encoding is set on it
  template <typename T>
  std::span<T> add(std::string_view name, std::size_t count) {
    auto &attribute = attributes[name];
    attribute.encoding = {};
    auto &values = attribute.storage.emplace<std::vector<T>>(count);
    return std::span<T>{values};
  }

  template <typename T> std::span<T> add(std::string_view name) {
    return add<T>(name, size());
  }

  // Moves each attribute value into the correct slot after filtering or
  // re-ordering point positions or colors.
  POINTCASTER_CORE_EXPORT void
  gather_attributes_into(PointCloud &destination,
                         std::span<const uint32_t> indices) const;
};

// the packed render buffer can either contain just positions and colors or
// additionally point scales too
inline size_t render_vertex_stride(const PointCloud &cloud) {
  return cloud.attributes.contains(point_scale_attribute) ? 20 : 16;
}

POINTCASTER_CORE_EXPORT PointCloud operator+(PointCloud const &lhs,
                                             PointCloud const &rhs);

// Appends rhs's points to lhs. An attribute held by only one of the two clouds
// is filled with defaults across the other's points.
POINTCASTER_CORE_EXPORT PointCloud &operator+=(PointCloud &lhs,
                                               const PointCloud &rhs);

using PointCloudPtr = std::shared_ptr<PointCloud>;

using PointCloudRef = std::reference_wrapper<PointCloud>;

constexpr bool operator==(const PointCloudRef &lhs, const PointCloudRef &rhs) {
  return std::addressof(lhs.get()) == std::addressof(rhs.get());
}
constexpr bool operator!=(const PointCloudRef &lhs, const PointCloudRef &rhs) {
  return !(rhs == lhs);
}

struct VoxelisedCloud : PointCloud {
  size_t voxel_size = 0;
};

using VoxelisedCloudPtr = std::shared_ptr<VoxelisedCloud>;

struct AabbList : PointCloud {
  // an aabb list needs two 'position' clouds...
  // one for min, one for max of the bounding box

  static constexpr std::string_view max_position_attribute = "max_position";

  AabbList() { add<position>(max_position_attribute, 0); }

  std::span<position> min_positions() { return positions; }
  std::span<position> max_positions() {
    return get<position>(max_position_attribute);
  }
  std::span<const position> min_positions() const { return positions; }
  std::span<const position> max_positions() const {
    return get<position>(max_position_attribute);
  }
};

using AabbListPtr = std::shared_ptr<AabbList>;

template <class T>
inline constexpr bool is_cloud_stream_v =
    std::is_same_v<T, PointCloudPtr> || std::is_same_v<T, VoxelisedCloudPtr> ||
    std::is_same_v<T, AabbListPtr>;

// this Archive serialize stuff is needed to make these types compatible
// with zpp_bits for serializing before publishing over zmq
template <typename Archive>
constexpr auto serialize(Archive &archive, VoxelisedCloud &cloud) {
  return archive(cloud.positions, cloud.colors, cloud.bounds, cloud.attributes,
                 cloud.voxel_size);
}
template <typename Archive>
constexpr auto serialize(Archive &archive, const VoxelisedCloud &cloud) {
  return archive(cloud.positions, cloud.colors, cloud.bounds, cloud.attributes,
                 cloud.voxel_size);
}

// AabbList has a user-provided constructor, so it is not an aggregate and
// zpp_bits cannot reflect over it the way it does PointCloud
template <typename Archive>
constexpr auto serialize(Archive &archive, AabbList &list) {
  return archive(list.positions, list.colors, list.bounds, list.attributes);
}
template <typename Archive>
constexpr auto serialize(Archive &archive, const AabbList &list) {
  return archive(list.positions, list.colors, list.bounds, list.attributes);
}

} // namespace pc