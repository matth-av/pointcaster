#pragma once

#include <codec/codec_config.h>
#include <string>
#include <vector>

namespace pc::networking {

struct StreamChannelConfiguration {
  std::string address;
  bool enabled = true;
};

struct PointStreamerConfiguration {
  std::string address = "*";
  int port = 9992; // @minmax(1024, 49151)
  int publish_hz = 60;
  bool publish_every_frame = false;
  CodecConfiguration codec_config;

  std::vector<StreamChannelConfiguration> channels{}; // @hidden
};

} // namespace pc::networking