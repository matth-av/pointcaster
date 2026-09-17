#pragma once

#include "config/group_sequence_config.h"
#include "config/transform_config.h"
#include <string>

namespace pc::devices {
struct DeviceGroupConfiguration {
  std::string id;         // @hidden
  std::string label;      // @hidden
  bool collapsed = false; // @hidden
  std::string parent_id;  // @hidden
  int order = 0;          // @hidden
  TransformConfiguration transform;
  GroupSequenceConfiguration sequence;
};
} // namespace pc::devices