#pragma once

#include "device_group_config.h"
#include "orbbec/orbbec_device_config.h"
#include "ply/ply_device_config.h"
#include <concepts>
#include <config/config_variant.h>
#include <optional>
#include <plugins/operators/operator_variants.h>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pc::devices {

// TODO we absolutely cannot be doing compile time variants for polymorphism,
// when we need the DeviceConfigurations to be able to be dynamically loaded
// from plugins

// we can't garuntee that all variants live inside this codebase
using DeviceConfigurationVariant =
    std::variant<OrbbecDeviceConfiguration, PlyDeviceConfiguration,
                 DeviceGroupConfiguration>;

// compile time utilities

template <typename Callback>
constexpr void for_each_device_config_type(Callback cb) {
  constexpr std::size_t device_config_count =
      std::variant_size_v<DeviceConfigurationVariant>;
  [&]<std::size_t... Indices>(std::index_sequence<Indices...>) {
    (cb.template operator()<
         std::variant_alternative_t<Indices, DeviceConfigurationVariant>>(),
     ...);
  }(std::make_index_sequence<device_config_count>{});
}

constexpr auto device_id_from_variant(const DeviceConfigurationVariant &v) {
  return std::visit([](const auto &cfg) { return std::string_view(cfg.id); },
                    v);
}

constexpr auto device_label(const DeviceConfigurationVariant &v) {
  return std::visit([](const auto &cfg) { return std::string_view(cfg.label); },
                    v);
}

// the operator list a device hosts, or nullptr for device types that can't
// host operators
inline std::vector<operators::OperatorConfigurationVariant> *
device_operators(DeviceConfigurationVariant &v) {
  return std::visit(
      [](auto &cfg) -> std::vector<operators::OperatorConfigurationVariant> * {
        if constexpr (requires { cfg.operators; })
          return &cfg.operators;
        else
          return nullptr;
      },
      v);
}

constexpr auto device_info_from_variant(const DeviceConfigurationVariant &v) {
  return std::visit(
      [](const auto &cfg) {
        using T = std::decay_t<decltype(cfg)>;
        if constexpr (std::same_as<T, DeviceGroupConfiguration>) {
          return std::make_tuple(std::string(cfg.id),
                                 std::string("DeviceGroup"));
        } else {
          return std::make_tuple(std::string(cfg.id),
                                 std::string(cfg.PluginName));
        }
      },
      v);
}

} // namespace pc::devices
