#pragma once

namespace pc {

struct SequenceConfiguration {
  bool playing = true; // @hidden
  bool looping = true;
  int frame_rate = 30;
  int start_frame = 0;      // @minmax(0, 999999999)
  int current_frame = 0;    // @minmax(0, 999999999)
  int end_frame = -1;       // @minmax(-1, 999999999)
  int buffer_capacity = 60; // @minmax(1, 999999999)
  int prefetch_ahead = 30;  // @minmax(1, 999999999)
};

} // namespace pc
