#include "mqtt_client.h"
#include "pointcaster/point_cloud.h"
#include "publishers/mqtt/mqtt_client_config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <format>
#include <logger/logger.h>
#include <memory>
#include <mqtt/client.h>
#include <mqtt/message.h>
#include <msgpack.hpp>
#include <mutex>
#include <optional>
#include <profiling/profiling_zone.h>
#include <ranges>
#include <rfl/json.hpp>
#include <stop_token>
#include <string>
#include <type_traits>
#include <uuid/uuid.h>
#include <variant>
#include <vector>
#include <workspace/workspace.h>
#include <workspace/workspace_socket.h>

namespace pc::publishers {

using namespace std::chrono;
using namespace std::chrono_literals;
using namespace pc::profiling;

namespace {

// some intermediary structs needed to serialize complex types over mqtt
struct StructuredAabb {
  std::array<int16_t, 3> min;
  std::array<int16_t, 3> max;
  MSGPACK_DEFINE_MAP(min, max);
  const std::string id;
};
struct StructuredAabbList {
  std::vector<StructuredAabb> aabbs;
  MSGPACK_DEFINE_ARRAY(aabbs);
};

constexpr auto connect_timeout = 10s;
constexpr auto disconnect_timeout = 500ms;
constexpr auto publish_timeout = 2s;
constexpr auto connection_retry_interval = 5s;

} // namespace

class MqttConnection {
public:
  explicit MqttConnection(const MqttClientConfiguration &config)
      : _client(config.broker_uri, config.client_id) {
    _options.set_connect_timeout(connect_timeout);
    _client.set_timeout(publish_timeout);
    try_connect();
  }

  ~MqttConnection() {
    try {
      if (_client.is_connected()) _client.disconnect(disconnect_timeout);
      pc::logger()->info("Closed MQTT client");
    } catch (const mqtt::exception &e) {
      pc::logger()->error("MQTT disconnect exception: {}", e.get_message());
    }
  }

  MqttConnection(const MqttConnection &) = delete;
  MqttConnection &operator=(const MqttConnection &) = delete;
  MqttConnection(MqttConnection &&) = delete;
  MqttConnection &operator=(MqttConnection &&) = delete;

  // no-op until retry_interval has elapsed since the last attempt
  void try_connect() {
    if (steady_clock::now() < _next_connection_attempt) return;
    try {
      _client.connect(_options);
      pc::logger()->info("MQTT client connected to '{}' as '{}'",
                         _client.get_server_uri(), _client.get_client_id());
    } catch (const mqtt::exception &e) {
      pc::logger()->warn("MQTT connect to '{}' failed: {}",
                         _client.get_server_uri(), e.get_message());
    }
    _next_connection_attempt = steady_clock::now() + connection_retry_interval;
  }

  bool connected() const { return _client.is_connected(); }

  void publish(const mqtt::const_message_ptr &msg) { _client.publish(msg); }

  bool matches(const MqttClientConfiguration &config) const {
    return _client.get_server_uri() == config.broker_uri &&
           _client.get_client_id() == config.client_id;
  }

private:
  mqtt::client _client;
  mqtt::connect_options _options;
  steady_clock::time_point _next_connection_attempt;
};

MqttClient::MqttClient(Workspace &workspace)
    : _listener(*this, workspace, workspace.config_registry) {}

MqttClient::~MqttClient() = default;

void MqttClient::handle_config_change(
    std::string_view path, const MqttClientConfiguration &config_snapshot) {
  _auto_reconnect = config_snapshot.auto_reconnect;

  if (!config_snapshot.enabled) {
    _connection.reset();
    return;
  }
  if (_connection && _connection->matches(config_snapshot)) return;

  _connection.reset();
  try {
    _connection = std::make_unique<MqttConnection>(config_snapshot);
  } catch (const mqtt::exception &e) {
    pc::logger()->error("MQTT client error: {}", e.get_message());
  }
}

void MqttClient::tick() {
  if (_auto_reconnect && _connection && !_connection->connected()) {
    _connection->try_connect();
  }
}

void MqttClient::handle_update(const std::string_view path,
                               const ConfigValue &value,
                               const MqttClientConfiguration &config_snapshot) {

  if (!_connection || !_connection->connected()) return;

  using SerializationFormat = MqttClientConfiguration::SerializationFormat;
  using EmptyMessageHandling = MqttClientConfiguration::EmptyMessageHandling;

  const auto &serialize_as_structures = config_snapshot.serialize_as_structures;
  const auto &serialization_format = config_snapshot.serialization_format;
  const auto &send_retained = config_snapshot.send_retained;
  const auto &empty_message_handling = config_snapshot.empty_message_handling;

  const mqtt::string_ref topic(path.data(), path.size());

  std::visit(
      [&](auto &value) {
        ProfilingZone mqtt_zone("MqttClient::process_msg");

        using VariantType = std::decay_t<decltype(value)>;
        mqtt::message_ptr msg;
        bool payload_empty;
        {
          ProfilingZone mqtt_serialization_zone("MqttClient::serialize");

          if constexpr (std::is_convertible<VariantType, std::string>()) {
            msg = mqtt::make_message(topic, value);
            payload_empty = std::empty(value);
          } else if constexpr (std::is_arithmetic<VariantType>()) {
            msg = mqtt::make_message(topic, std::to_string(value));
            payload_empty = false;
          } else if constexpr (std::same_as<VariantType, AabbListPtr>) {
            if (!value) return;

            // we either serialize as typed aabb structs, which are easier to
            // read for instance in mqtt explorer as json, and may be easier
            // in some applications to decode, or we serialize as basic
            // array / list types of min/max bounds, which is more efficient

            if (serialize_as_structures) {
              const auto &min = value->min_positions();
              const auto &max = value->max_positions();
              // TODO cpp26...
              // const auto indices =
              // std::views::indices(std::ranges::size(min));
              const auto indices =
                  std::ranges::iota_view(0, static_cast<int>(value->size()));
              StructuredAabbList list{
                  .aabbs = std::views::zip(min, max, indices) |
                           std::views::transform([](const auto &aabb) {
                             const auto &[min, max, i] = aabb;
                             return StructuredAabb{
                                 .min = {min.x, min.y, min.z},
                                 .max = {max.x, max.y, max.z},
                                 // TODO id's are just indices for now
                                 .id = std::format("{}", i)};
                           }) |
                           std::ranges::to<std::vector>()};
              payload_empty = list.aabbs.empty();

              if (serialization_format == SerializationFormat::JSON) {
                ProfilingZone json_zone("serialize::json");
                msg = mqtt::make_message(topic, rfl::json::write(list));
              } else { // SerializationFormat::MessagePack
                ProfilingZone msgpack_zone("serialize::msgpack");
                msgpack::sbuffer send_buffer;
                msgpack::pack(send_buffer, list);
                msg = mqtt::make_message(topic, send_buffer.data(),
                                         send_buffer.size());
              }
            } else {
              using std_aabb = std::array<std::array<int16_t, 3>, 2>;
              const auto aabbs = std::views::zip(value->min_positions(),
                                                 value->max_positions()) |
                                 std::views::transform([](const auto &min_max) {
                                   const auto &[min, max] = min_max;
                                   return std_aabb{{{min.x, min.y, min.z},
                                                    {max.x, max.y, max.z}}};
                                 }) |
                                 std::ranges::to<std::vector>();
              payload_empty = aabbs.empty();

              if (serialization_format == SerializationFormat::JSON) {
                ProfilingZone json_zone("serialize::json");
                msg = mqtt::make_message(topic, rfl::json::write(aabbs));
              } else { // SerializationFormat::MessagePack
                ProfilingZone msgpack_zone("serialize::msgpack");
                msgpack::sbuffer send_buffer;
                msgpack::pack(send_buffer, aabbs);
                msg = mqtt::make_message(topic, send_buffer.data(),
                                         send_buffer.size());
              }
            }
          } else if constexpr (pc::is_cloud_stream_v<VariantType>) {
            if (!value) return;

            using std_position = std::array<int16_t, 3>;
            const auto positions = value->positions |
                                   std::views::transform([](const auto &p) {
                                     return std_position{{p.x, p.y, p.z}};
                                   }) |
                                   std::ranges::to<std::vector>();
            payload_empty = positions.empty();

            if (serialization_format == SerializationFormat::JSON) {
              ProfilingZone json_zone("serialize::json");
              msg = mqtt::make_message(topic, rfl::json::write(positions));
            } else { // SerializationFormat::MessagePack
              ProfilingZone msgpack_zone("serialize::msgpack");
              msgpack::sbuffer send_buffer;
              msgpack::pack(send_buffer, positions);
              msg = mqtt::make_message(topic, send_buffer.data(),
                                       send_buffer.size());
            }
          } else if constexpr (std::same_as<VariantType, position_bounds>) {
            // a bounds is one box, so it goes out in the shape a single
            // element of an aabb list would
            const std::array<int16_t, 3> min{value.min.x, value.min.y,
                                             value.min.z};
            const std::array<int16_t, 3> max{value.max.x, value.max.y,
                                             value.max.z};
            payload_empty = false;

            if (serialize_as_structures) {
              const StructuredAabb bounds{
                  .min = min, .max = max, .id = "bounds"};
              if (serialization_format == SerializationFormat::JSON) {
                ProfilingZone json_zone("serialize::json");
                msg = mqtt::make_message(topic, rfl::json::write(bounds));
              } else { // SerializationFormat::MessagePack
                ProfilingZone msgpack_zone("serialize::msgpack");
                msgpack::sbuffer send_buffer;
                msgpack::pack(send_buffer, bounds);
                msg = mqtt::make_message(topic, send_buffer.data(),
                                         send_buffer.size());
              }
            } else {
              const std::array<std::array<int16_t, 3>, 2> bounds{min, max};
              if (serialization_format == SerializationFormat::JSON) {
                ProfilingZone json_zone("serialize::json");
                msg = mqtt::make_message(topic, rfl::json::write(bounds));
              } else { // SerializationFormat::MessagePack
                ProfilingZone msgpack_zone("serialize::msgpack");
                msgpack::sbuffer send_buffer;
                msgpack::pack(send_buffer, bounds);
                msg = mqtt::make_message(topic, send_buffer.data(),
                                         send_buffer.size());
              }
            }
          } else if constexpr (std::same_as<VariantType, radius> ||
                               std::same_as<VariantType, length>) {
            auto msg_str = std::format("{}", value.mm);
            payload_empty = false;
            msg = mqtt::make_message(topic, std::move(msg_str));
          } else {
            const auto msg_str = std::format("{}", value);
            payload_empty = msg_str.empty();
            msg = mqtt::make_message(topic, std::move(msg_str));
          }
        }
        {
          ProfilingZone mqtt_publish_zone("MqttClient::publish");

          static thread_local bool has_published_empty_once = false;
          msg->set_retained(send_retained);

          try {
            if (!payload_empty) {
              _connection->publish(msg);
              has_published_empty_once = false;
            } else if (empty_message_handling ==
                       EmptyMessageHandling::PublishEmptyAlways) {
              _connection->publish(msg);
            } else if (empty_message_handling ==
                           EmptyMessageHandling::PublishEmptyOnce &&
                       !has_published_empty_once) {
              _connection->publish(msg);
              has_published_empty_once = true;
            }
          } catch (const mqtt::exception &e) {
            pc::logger()->error("MQTT publish failed with exception: {}",
                                e.get_message());
          }
        }
      },
      value);
}

MqttClientConfiguration MqttClient::config(Workspace &workspace) const {
  std::scoped_lock lock(workspace.config_access);
  return workspace.config.publishers.mqtt;
}

} // namespace pc::publishers