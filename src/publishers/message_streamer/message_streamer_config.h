#pragma once

#include <rfl/Literal.hpp>

namespace pc::publishers {

struct MessageStreamerConfiguration {
  bool enabled = true;
  std::string interface_ip = "0.0.0.0";
  int port = 9991; // @minmax(1024, 49151)

  using Tag = rfl::Literal<"message_streamer">;
};

} // namespace pc::publishers