#pragma once

#include <codec/codec_config.h>
#include <rfl/DefaultVal.hpp>
#include <string>
#include <vector>

namespace pc::networking {

struct StreamChannelConfiguration {
  std::string address;
  rfl::DefaultVal<bool> enabled = true;
};

struct PointStreamerConfiguration {
  rfl::DefaultVal<std::string> address = "*";
  rfl::DefaultVal<int> port = 9992; // @minmax(1024, 49151)
  rfl::DefaultVal<int> publish_hz = 60;
  rfl::DefaultVal<bool> publish_every_frame = false;
  rfl::DefaultVal<CodecConfiguration> codec_config;

  std::vector<StreamChannelConfiguration> channels{}; // @hidden
};

} // namespace pc::networking