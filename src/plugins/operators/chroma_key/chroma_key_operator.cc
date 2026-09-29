#include "chroma_key_operator.h"
#include "chroma_key_cpu.h"

#include <exception>
#include <memory>
#include <profiling/profiling_zone.h>

namespace pc::operators {

void ChromaKeyOperator::init(
    OperatorHost *host, Corrade::PluginManager::Manager<backend::BackendPlugin>
                            &backend_plugin_manager) {
  OperatorPlugin::init(host, backend_plugin_manager);
  pc::logger()->trace("Initialised ChromaKeyOperator");
}

PipelineFramePtr ChromaKeyOperator::process(PipelineFramePtr input) {
  using profiling::ProfilingZone;

  auto variant = load_config();
  if (!variant) return input;
  const auto &config = std::get<ChromaKeyConfiguration>(*variant);

  if (!input->cloud || input->cloud->empty()) return input;

  ProfilingZone operator_zone("ChromaKeyOperator");
  operator_zone.text(config.id);

  const chroma_key::KeyParameters key{
      .target_hue_degrees = config.target_hue_degrees,
      .hue_width_degrees = config.hue_width_degrees,
      .minimum_saturation = config.minimum_saturation,
      .minimum_value = config.minimum_value,
      .invert = config.invert};

  auto keyed_cloud = std::make_shared<PointCloud>();

  // the backend we resolve to is CUDA only when the cuda backend plugin loaded,
  // so its presence stands in for a usable gpu
  if (current_backend_type() == BackendType::CUDA) {
    try {
      _cuda_key.key_cloud(*input->cloud, *keyed_cloud, key);
    } catch (const std::exception &e) {
      pc::logger()->error("ChromaKeyOperator CUDA error: {}", e.what());
      return input;
    }
  } else {
    chroma_key::key_cloud_cpu(*input->cloud, *keyed_cloud, key);
  }

  auto output_frame = input->clone();
  output_frame->cloud = std::move(keyed_cloud);
  return output_frame;
}

} // namespace pc::operators

CORRADE_PLUGIN_REGISTER(ChromaKeyOperator, pc::operators::ChromaKeyOperator,
                        "net.pointcaster.OperatorPlugin/1.0")
