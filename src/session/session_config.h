#pragma once

#include <camera/camera_config.h>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <plugins/operators/operator_variants.h>
#include <set>
#include <string>
#include <vector>

namespace pc {

struct SessionTimelineConfiguration {
  bool looping = true;
  int fps = 30;
  int start_frame = 0;   // @minmax(0, 999999999)
  int current_frame = 0; // @minmax(0, 999999999)
  int end_frame = -1;    // @minmax(-1, 999999999);
  int length = -1;       // @minmax(-1, 999999999);
};

struct SessionConfiguration {
  std::string id;                        // @hidden
  std::string label;                     // @hidden
  CameraConfiguration camera;            // @folded
  SessionTimelineConfiguration timeline; // @hidden
  operators::ConcurrentOperatorPipelineConfiguration
      operator_pipeline;                                          // @folded
  std::vector<operators::OperatorConfigurationVariant> operators; // @hidden
  std::set<std::string> disabled_devices;                         // @hidden
};

inline std::string session_address(const SessionConfiguration &config) {
  const auto &label = config.label;
  return !label.empty() ? label : config.id;
}

} // namespace pc
