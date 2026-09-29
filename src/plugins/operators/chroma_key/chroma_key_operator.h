#pragma once

#include "chroma_key_config.h"
#include "chroma_key_cuda.h"

#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <plugins/backend/backend_plugin.h>
#include <plugins/operators/operator_plugin.h>

namespace pc::operators {

// removes points whose colour falls inside a band of hue, like a green
// screen key
class ChromaKeyOperator final : public OperatorPlugin {
public:
  explicit ChromaKeyOperator(Corrade::PluginManager::AbstractManager &manager,
                             Corrade::Containers::StringView plugin)
      : OperatorPlugin(manager, plugin) {}

  ~ChromaKeyOperator() override {}

  ChromaKeyOperator(const ChromaKeyOperator &) = delete;
  ChromaKeyOperator &operator=(const ChromaKeyOperator &) = delete;
  ChromaKeyOperator(ChromaKeyOperator &&) = delete;
  ChromaKeyOperator &operator=(ChromaKeyOperator &&) = delete;

  void init(OperatorHost *host,
            Corrade::PluginManager::Manager<backend::BackendPlugin>
                &backend_plugin_manager) override;

  PipelineFramePtr process(PipelineFramePtr input) override;

  const ChromaKeyConfiguration &config() const {
    return std::get<ChromaKeyConfiguration>(config_variant());
  }

private:
  chroma_key::CudaChromaKey _cuda_key;
};

} // namespace pc::operators
