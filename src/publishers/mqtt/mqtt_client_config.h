#pragma once
#include <rfl/Literal.hpp>
#include <string>

namespace pc::publishers {

struct MqttClientConfiguration {
  bool enabled = false;
  std::string broker_uri = "tcp://localhost:1884";
  std::string client_id = "pointcaster";
  bool auto_reconnect = true;

  // how a value that isn't a plain scalar or string gets encoded
  enum class SerializationFormat { JSON, MessagePack };
  SerializationFormat serialization_format = SerializationFormat::JSON;

  bool serialize_as_structures = false;
  bool send_retained = false;

  enum class EmptyMessageHandling {
    Ignore,
    PublishEmptyOnce,
    PublishEmptyAlways
  };
  EmptyMessageHandling empty_message_handling = EmptyMessageHandling::Ignore;

  using Tag = rfl::Literal<"mqtt">;
};

} // namespace pc::publishers
