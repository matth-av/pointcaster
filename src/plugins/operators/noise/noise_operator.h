#pragma once

#include "noise_config.h"
#include "noise_cuda.h"

#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <plugins/backend/backend_plugin.h>
#include <plugins/operators/operator_plugin.h>

namespace pc::operators {

// moves each point by a fractal noise field sampled where it sits
class NoiseOperator final : public OperatorPlugin {
public:
  explicit NoiseOperator(Corrade::PluginManager::AbstractManager &manager,
                         Corrade::Containers::StringView plugin)
      : OperatorPlugin(manager, plugin) {}

  ~NoiseOperator() override {}

  NoiseOperator(const NoiseOperator &) = delete;
  NoiseOperator &operator=(const NoiseOperator &) = delete;
  NoiseOperator(NoiseOperator &&) = delete;
  NoiseOperator &operator=(NoiseOperator &&) = delete;

  void init(OperatorHost *host,
            Corrade::PluginManager::Manager<backend::BackendPlugin>
                &backend_plugin_manager) override;

  PipelineFramePtr process(PipelineFramePtr input) override;

  const NoiseConfiguration &config() const {
    return std::get<NoiseConfiguration>(config_variant());
  }

private:
  noise::CudaNoise _cuda_noise;
};

} // namespace pc::operators
