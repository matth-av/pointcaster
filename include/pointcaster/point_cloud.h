#pragma once

#include "core_types.h"
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

class PointCloud {
public:
  std::vector<position> positions;
  std::vector<color> colors;

  position_bounds bounds;

  // Arbitrary per-point attributes, keyed by name.
  using attribute_storage =
      std::variant<std::vector<float>, std::vector<scale>,
                   std::vector<position>>;
  StringMap<attribute_storage> attributes;

  auto size() const { return positions.size(); }
  auto empty() const { return positions.empty(); }

  void resize(std::size_t new_size) {
    positions.resize(new_size);
    colors.resize(new_size);
    for (auto &[_, storage] : attributes) {
      std::visit([new_size](auto &values) { values.resize(new_size); },
                 storage);
    }
  }

  void reserve(std::size_t new_capacity) {
    positions.reserve(new_capacity);
    colors.reserve(new_capacity);
    for (auto &[_, storage] : attributes) {
      std::visit([new_capacity](auto &values) { values.reserve(new_capacity); },
                 storage);
    }
  }

  POINTCASTER_CORE_EXPORT std::vector<std::byte>
  serialize(bool compress = false) const;

  POINTCASTER_CORE_EXPORT static PointCloud
  deserialize(std::span<const std::byte> buffer);

  // Return an attribute by name
  template <typename T>
  [[nodiscard]] std::span<T> get(std::string_view name) noexcept {
    auto it = attributes.find(name);
    if (it == attributes.end()) return {};
    auto *values = std::get_if<std::vector<T>>(&it->second);
    return values ? std::span<T>{*values} : std::span<T>{};
  }

  template <typename T>
  [[nodiscard]] std::span<const T> get(std::string_view name) const noexcept {
    return const_cast<PointCloud *>(this)->get<T>(name);
  }

  // Creates or replaces an attribute with one entry per point and hands back
  // the span to fill in.
  template <typename T>
  std::span<T> add(std::string_view name, std::size_t count) {
    // emplace hands back a reference to the alternative it just built
    auto &values = attributes[std::string{name}].emplace<std::vector<T>>(count);
    return std::span<T>{values};
  }

  template <typename T> std::span<T> add(std::string_view name) {
    return add<T>(name, size());
  }

  // Moves each attribute value into the slot its point ended up in.
  // Needs to be called after any filtering of points and before resizing the
  // destination. indices names the survivors in the order they should end up
  POINTCASTER_CORE_EXPORT void
  gather_attributes_into(PointCloud &destination,
                         std::span<const uint32_t> indices) const;

private:
  std::vector<std::byte> compress() const;
  static PointCloud decompress(const std::vector<std::byte> &buffer,
                               unsigned long point_count);
};

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

  // accessors to make this clearer at the call site
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

struct PointCloudPacket {
  // out packet needs these explicitly sized types to ensure portability
  // between unix and windows systems
  uint64_t timestamp;
  uint64_t point_count;
  uint8_t compressed;
  std::vector<std::byte> data;

  static constexpr std::size_t header_bytes =
      sizeof(uint64_t)   // timestamp
      + sizeof(uint64_t) // point_count
      + sizeof(uint8_t); // compressed flag
};

} // namespace pc