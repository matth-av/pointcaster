#pragma once

#include "color_transform_config.h"
#include "file_config.h"
#include "group_sequence_config.h"
#include "network_config.h"
#include "publishers/message_streamer/message_streamer_config.h"
#include "sequence_config.h"
#include "transform_config.h"

#include <concepts>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <point_streamer/point_streamer_config.h>
#include <type_traits>
#include <utility>
#include <variant>

// TODO REMOVE AND ADD
// specialised jinja with  setConfig overload like device adapters
#include <plugins/operators/cluster_extraction/cluster_extraction_config.h>
#include <plugins/operators/fringe_removal/fringe_removal_config.h>
#include <plugins/operators/range_filter/range_filter_config.h>

#include <camera/camera_config.h>
#include <camera/look_at_camera_config.h>
#include <codec/codec_config.h>
#include <config/transform_config.h>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <publishers/mqtt/mqtt_client_config.h>
#include <publishers/osc/osc_sender_config.h>
#include <publishers/publishers_config.h>
#include <session/session_config.h>

namespace pc {

using ConfigurationVariant = std::variant<
    CodecConfiguration, TransformConfiguration, ColorTransformConfiguration,
    FileConfiguration,
    FileFolderConfiguration, NetworkConfiguration, CameraConfiguration,
    LookAtCameraConfiguration, SessionConfiguration,
    SessionTimelineConfiguration, SequenceConfiguration,
    GroupSequenceConfiguration,
    operators::ConcurrentOperatorPipelineConfiguration,
    networking::PointStreamerConfiguration,
    networking::StreamChannelConfiguration, publishers::PublishersConfiguration,
    publishers::MessageStreamerConfiguration,
    publishers::MqttClientConfiguration, publishers::OscSenderConfiguration,
    // TODO REMOVE AND ADD
    // specialised jinja with  setConfig overload like device
    // adapters
    operators::FringeRemovalConfiguration,
    operators::ClusterExtractionConfiguration,
    operators::RangeFilterConfiguration>;

// compile time utilities

template <typename T, typename Variant> struct is_in_variant;

template <typename T, typename... Ts>
struct is_in_variant<T, std::variant<Ts...>>
    : std::bool_constant<(std::same_as<T, Ts> || ...)> {};

template <typename T, typename Variant>
inline constexpr bool is_in_variant_v = is_in_variant<T, Variant>::value;

template <typename Callback, typename Variant>
constexpr void for_each_variant_type(Callback cb) {
  constexpr std::size_t count = std::variant_size_v<Variant>;
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (cb.template operator()<std::variant_alternative_t<I, Variant>>(), ...);
  }(std::make_index_sequence<count>{});
}

template <typename T>
concept ValidConfig = is_in_variant_v<T, ConfigurationVariant>;

template <typename Callback>
constexpr void for_each_core_config_type(Callback cb) {
  for_each_variant_type<Callback, ConfigurationVariant>(cb);
}

} // namespace pc
