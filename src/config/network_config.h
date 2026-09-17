#pragma once

#include <rfl/Skip.hpp>
#include <string>

namespace pc {

struct NetworkConfiguration {
  std::string ip_address = "";
  std::string subnet_mask = "255.255.255.0";
  std::string gateway_address = "192.168.1.1";
  rfl::Skip<bool> apply;
};

} // namespace pc
