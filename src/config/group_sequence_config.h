#pragma once

namespace pc {
struct GroupSequenceConfiguration {
  bool playing = true;   // @hidden
  bool looping = true;   // @hidden
  int current_frame = 0; // @hidden
};
} // namespace pc
