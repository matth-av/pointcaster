#pragma once
#include <rfl/Literal.hpp>
#include <string>

namespace pc::publishers {

struct OscSenderConfiguration {
  bool enabled = false;
  std::string host = "127.0.0.1";
  int port = 9000; // @minmax(1024, 49151)

  // positions can either be sent as raw millimeter shorts, or encoded by this
  // sender as metre floats
  enum class PositionEncoding { Millimetres, Metres };
  PositionEncoding position_encoding = PositionEncoding::Millimetres;

  // a point cloud fans out into one message per point at '<path>/<index>',
  // so this caps how many of those go out for any single cloud...
  // (which shouldn't really be many when we're talking about OSC here)
  int max_cloud_points = 16; // @minmax(1, 256)

  using Tag = rfl::Literal<"osc">;
};

} // namespace pc::publishers
