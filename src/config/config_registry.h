#pragma once

#include "config_value.h"
#include "output_value.h"

#include <pointcaster/config.h>
#include <pointcaster/core_types.h>
#include <pointcaster/point_cloud.h>
#include <rfl/Skip.hpp>
#include <rfl/internal/to_ptr_named_tuple.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <util/string_map.h>
#include <variant>
#include <vector>

namespace pc {

class Workspace;

class ConfigRegistry {
public:
  struct Field {
    std::function<ConfigValue()> get;
    std::function<void(ConfigValue)> set;
  };

  POINTCASTER_CONFIG_EXPORT void register_field(std::string path, Field field);

  // Remove all registered paths that begin with prefix
  POINTCASTER_CONFIG_EXPORT void unregister(std::string_view prefix);

  POINTCASTER_CONFIG_EXPORT void clear();

  // returns false if path not found, or if the field is read-only
  [[maybe_unused]] POINTCASTER_CONFIG_EXPORT bool set(std::string_view path,
                                                      ConfigValue value);

  POINTCASTER_CONFIG_EXPORT bool is_readonly(std::string_view path) const;

  POINTCASTER_CONFIG_EXPORT void notify(std::string_view path);

  POINTCASTER_CONFIG_EXPORT std::optional<ConfigValue>
  get(std::string_view path) const;

  // batch reads a whole set of paths in one pass, returning a snapshot
  template <typename StringCollection>
  void snapshot(const StringCollection &paths, StringMap<ConfigValue> &out);

  using ChangeCallback = std::function<void(std::string_view path)>;
  using SubscriptionId = std::uint64_t;

  POINTCASTER_CONFIG_EXPORT SubscriptionId on_change(std::string_view prefix,
                                                     ChangeCallback cb);

  // ids are never reused, so removing one that's already gone is a no-op
  // rather than a match against some later subscription
  POINTCASTER_CONFIG_EXPORT void remove_subscription(SubscriptionId id);

private:
  struct Subscription {
    SubscriptionId id;
    std::string prefix;
    ChangeCallback callback;
  };

  mutable std::shared_mutex _mutex;
  StringMap<Field> _fields;
  std::vector<Subscription> _subs;
  SubscriptionId _next_subscription_id = 1;
};

// ---- rfl wrapper type traits ----

template <class T> struct is_rfl_skip : std::false_type {};
template <class T> struct is_rfl_skip<rfl::Skip<T>> : std::true_type {};
template <class T> inline constexpr bool is_rfl_skip_v = is_rfl_skip<T>::value;

template <class T> struct is_toggleable : std::false_type {};
template <class T> struct is_toggleable<Toggleable<T>> : std::true_type {};
template <class T>
inline constexpr bool is_toggleable_v = is_toggleable<T>::value;

// Types that map to a ConfigValue variant arm directly
template <class T>
concept ConfigValueCompatible =
    std::is_same_v<T, bool> || std::is_same_v<T, int> ||
    std::is_same_v<T, float> || std::is_same_v<T, double> ||
    std::is_same_v<T, std::string> || std::is_enum_v<T>;

// Types that sit at a registry path as one whole value, rather than being
// walked into for the fields underneath them
template <class T>
concept RegistryLeaf =
    ConfigValueCompatible<T> || std::is_same_v<T, position_bounds> ||
    std::is_same_v<T, radius>;

// Types rfl can traverse recursively (plain aggregates; excludes arrays,
// std::string, std::vector, etc. which are not aggregates)
template <class T>
concept RflTraversable = std::is_aggregate_v<T> && !std::is_array_v<T>;

// A tagged union is not an aggregate, so the field walk cannot see into one
// the way it sees into a nested configuration
template <class T>
concept RflTaggedUnion = requires(T &t) {
  typename T::VariantType;
  { t.variant() } -> std::convertible_to<typename T::VariantType &>;
};

// ---- Value conversion ----

template <class T> ConfigValue to_config_value(const T &v) {
  if constexpr (std::is_same_v<T, bool>)
    return v;
  else if constexpr (std::is_same_v<T, int>)
    return v;
  else if constexpr (std::is_same_v<T, float>)
    return v;
  else if constexpr (std::is_same_v<T, double>)
    return v;
  else if constexpr (std::is_same_v<T, std::string>)
    return v;
  else if constexpr (std::is_enum_v<T>)
    return static_cast<int>(v);
  else if constexpr (std::is_same_v<T, position_bounds>)
    return v;
  else if constexpr (std::is_same_v<T, radius>)
    return v;
  else if constexpr (is_cloud_stream_v<T>)
    return v;
  else
    static_assert(sizeof(T) == 0, "to_config_value: unsupported type");
}

template <class T> T from_config_value(const ConfigValue &v) {
  if constexpr (std::is_enum_v<T>) {
    // enums come in as int, but accept any numeric
    return std::visit(
        [](auto &&val) -> T {
          using V = std::decay_t<decltype(val)>;
          if constexpr (std::is_arithmetic_v<V>)
            return static_cast<T>(static_cast<int>(val));
          return T{};
        },
        v);
  } else if constexpr (std::is_same_v<T, bool>) {
    return std::visit(
        [](auto &&val) -> bool {
          using V = std::decay_t<decltype(val)>;
          if constexpr (std::is_arithmetic_v<V>) return static_cast<bool>(val);
          if constexpr (std::is_same_v<V, std::string>) return !val.empty();
          return false;
        },
        v);
  } else if constexpr (std::is_arithmetic_v<T>) {
    // int/float/double all coerce freely to each other
    return std::visit(
        [](auto &&val) -> T {
          using V = std::decay_t<decltype(val)>;
          if constexpr (std::is_arithmetic_v<V>) return static_cast<T>(val);
          if constexpr (std::is_same_v<V, std::string>) {
            try {
              if constexpr (std::is_floating_point_v<T>)
                return static_cast<T>(std::stod(val));
              else
                return static_cast<T>(std::stol(val));
            } catch (...) {
              return T{};
            }
          }
          return T{};
        },
        v);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return std::visit(
        [](auto &&val) -> std::string {
          using V = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<V, std::string>) return val;
          if constexpr (std::is_same_v<V, bool>) return val ? "true" : "false";
          if constexpr (std::is_arithmetic_v<V>) return std::to_string(val);
          return {};
        },
        v);
  } else if constexpr (std::is_same_v<T, position_bounds>) {
    const auto *held = std::get_if<T>(&v);
    return held ? *held : T{};
  } else if constexpr (std::is_same_v<T, radius>) {
    // a bare number coming the other way is read as the millimetre count it
    // would have serialised as
    return std::visit(
        [](auto &&val) -> radius {
          using V = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<V, radius>) return val;
          if constexpr (std::is_arithmetic_v<V>)
            return radius::from_millimetres(static_cast<float>(val));
          return radius{};
        },
        v);
  } else {
    return std::get<T>(v);
  }
}

// ---- register_config ----

// Forward declaration so nested calls compile
template <class T>
void register_config(ConfigRegistry &reg, std::string_view prefix, T &cfg);

// A Toggleable keeps the shape its inner value already had...
//  the wrapper's own path still reads and writes that value, and the switch
//  sits beside it under its own "/active".
template <class Switched, class Read, class Write>
void register_toggleable(ConfigRegistry &reg, const std::string &path,
                         Read read, Write write) {
  using Value = typename Switched::value_type;

  reg.register_field(
      path, {[read]() -> ConfigValue { return to_config_value(read().value); },
             [read, write](ConfigValue v) {
               auto held = read();
               held.value = from_config_value<Value>(v);
               write(held);
             }});

  reg.register_field(
      path + "/active",
      {[read]() -> ConfigValue { return ConfigValue(read().active); },
       [read, write](ConfigValue v) {
         auto held = read();
         held.active = from_config_value<bool>(v);
         write(held);
       }});
}

template <class Variant>
void register_variant(ConfigRegistry &reg, const std::string &path,
                      Variant &variant) {
  variant.visit([&](auto &alternative) {
    using Alt = std::decay_t<decltype(alternative)>;
    // an alternative with no tag has no name to sit under
    if constexpr (requires { typename Alt::Tag; }) {
      register_config(reg, path + "/" + Alt::Tag::strings().at(0), alternative);
    }
  });
}

template <class T>
void register_config(ConfigRegistry &reg, std::string_view prefix, T &cfg) {
  auto ptr_nt = rfl::internal::to_ptr_named_tuple(cfg);
  ptr_nt.apply([&](auto field) {
    using FieldType = std::decay_t<decltype(field)>;
    using ValPtrType = typename FieldType::Type;       // e.g. int*
    using ValType = std::remove_pointer_t<ValPtrType>; // e.g. int

    if constexpr (is_rfl_skip_v<ValType>) return;

    const std::string path =
        std::string(prefix) + "/" + std::string(FieldType::name());
    ValType *ptr = field.value_;

    if constexpr (is_output_v<ValType>) {
      reg.register_field(path, {[ptr]() -> ConfigValue {
                                  return to_config_value(ptr->value());
                                },
                                nullptr});
      // writes to the registry not coming fron set just notify changes
      ptr->bind([&reg, path] { reg.notify(path); });
      return;
    }

    if constexpr (is_toggleable_v<ValType>) {
      if constexpr (RegistryLeaf<typename ValType::value_type>) {
        register_toggleable<ValType>(
            reg, path, [ptr] { return *ptr; },
            [ptr](const ValType &held) { *ptr = held; });
      }
    } else if constexpr (std::is_same_v<ValType, position_bounds>) {
      reg.register_field(
          path,
          {[ptr]() -> ConfigValue { return to_config_value(*ptr); },
           [ptr](ConfigValue v) { *ptr = from_config_value<ValType>(v); }});
    } else if constexpr (RflTaggedUnion<ValType>) {
      register_variant(reg, path, ptr->variant());
    } else if constexpr (RflTraversable<ValType>) {
      register_config(reg, path, *ptr);
    } else if constexpr (ConfigValueCompatible<ValType>) {
      reg.register_field(
          path,
          {[ptr]() -> ConfigValue { return to_config_value(*ptr); },
           [ptr](ConfigValue v) { *ptr = from_config_value<ValType>(v); }});
    }
    // else: std::vector, std::unordered_map, etc. — skip silently
  });
}

} // namespace pc