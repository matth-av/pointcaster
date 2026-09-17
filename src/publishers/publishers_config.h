#pragma once
#include "message_streamer/message_streamer_config.h"
#include "mqtt/mqtt_client_config.h"
#include "osc/osc_sender_config.h"

namespace pc::publishers {

struct PublishersConfiguration {
  int publish_hz = 100; // @minmax(5, 200)

  MessageStreamerConfiguration message_streamer;
  MqttClientConfiguration mqtt;
  OscSenderConfiguration osc;
};

} // namespace pc::publishers
