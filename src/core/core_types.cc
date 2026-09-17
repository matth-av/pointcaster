#include <pointcaster/core_types.h>

#include <type_traits>
#include <zpp_bits.h>

namespace pc {

// all of this ensures that fast-paths can be taken on de/serializing and
// encoding/decoding point cloud structures

static_assert(std::is_trivially_copyable_v<position> &&
              std::has_unique_object_representations_v<position> &&
              sizeof(position) == 8);

static_assert(std::is_trivially_copyable_v<color> &&
              std::has_unique_object_representations_v<color> &&
              sizeof(color) == 4);

static_assert(std::is_trivially_copyable_v<AttributeEncoding> &&
              sizeof(AttributeEncoding) == 8);

static_assert(sizeof(length) == 2 && sizeof(radius) == 2);
static_assert(length::from_metres(1.0f).mm == 1000);
static_assert(length::from_millimetres(-1.4f).mm == -1);
static_assert(radius::from_centimetres(5.0f).mm == 50);
static_assert(radius::from_millimetres(-5.0f).mm == 0);

static_assert(zpp::bits::concepts::byte_serializable<position>);
static_assert(zpp::bits::concepts::byte_serializable<color>);
static_assert(zpp::bits::concepts::byte_serializable<AttributeEncoding>);

} // namespace pc