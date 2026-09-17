#pragma once
#include <Corrade/Containers/Pointer.h>
#include <memory>
#include <mutex>
#include <pipeline/concurrent_operator_pipeline_config.h>
#include <pipeline/pipeline_frame.h>
#include <plugins/operators/operator_variants.h>
#include <pointcaster/point_cloud.h>
#include <span>
#include <string_view>
#include <vector>

namespace pc {
class Workspace;
}
namespace pc::operators {
class ConcurrentOperatorPipeline;
}

namespace pc::operators {

class OperatorPlugin;

class OperatorHost {
public:
  virtual ~OperatorHost();
  // Ask the host to re-run its operator pipeline on the current input.
  virtual void reprocess() = 0;

  void init(Workspace &workspace) { _workspace = &workspace; }

  bool has_workspace() const noexcept { return _workspace != nullptr; }

  std::vector<Corrade::Containers::Pointer<OperatorPlugin>> operators{};

  void feed_operator_pipeline(std::shared_ptr<PointCloud> cloud);

  void update_operator_in_pipeline(const OperatorConfigurationVariant &config,
                                   std::string_view changed_path);

  size_t pipeline_concurrency() { return pipeline_config().concurrency; }

protected:
  Workspace *_workspace = nullptr;

  std::mutex _pipeline_mutex;
  std::unique_ptr<operators::ConcurrentOperatorPipeline> _pipeline;

  void sync_operators(std::span<const OperatorConfigurationVariant> configs);
  void rebuild_pipeline(std::span<const OperatorConfigurationVariant> configs);

  virtual operators::ConcurrentOperatorPipelineConfiguration &
  pipeline_config() = 0;

  virtual void on_pipeline_output(PipelineFramePtr) {}
};

} // namespace pc::operators