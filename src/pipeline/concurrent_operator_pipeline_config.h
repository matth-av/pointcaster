#pragma once

namespace pc::operators {

struct ConcurrentOperatorPipelineConfiguration {
  int update_hz = 120;
  int concurrency = 4;
};

} // namespace pc::operators