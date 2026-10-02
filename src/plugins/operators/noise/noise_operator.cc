#include "noise_operator.h"
#include "noise_cpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <profiling/profiling_zone.h>
#include <random>

namespace pc::operators {

namespace {

noise::DisplacementParameters
displacement_parameters(const NoiseConfiguration &config) {
  const float lacunarity = std::clamp(config.lacunarity, 1.0f, 4.0f);
  const float decay = std::clamp(config.decay, 0.0f, 1.0f);
  // with no decay the'res only 1 octave
  const int octaves =
      decay > 0.0f ? std::clamp(config.octaves, 1, noise::max_octaves) : 1;
  const auto feature_size =
      static_cast<float>(std::max<int16_t>(1, config.feature_size.mm));
  const bool perlin = config.algorithm == NoiseConfiguration::Algorithm::Perlin;
  const bool periodic = perlin && config.period.active;
  const auto period = static_cast<float>(std::max(1, config.period.value));

  noise::DisplacementParameters displacement{
      .algorithm = periodic ? noise::Algorithm::PeriodicPerlin
                   : perlin ? noise::Algorithm::Perlin
                            : noise::Algorithm::Simplex,
      .magnitude = static_cast<float>(config.magnitude.mm),
      .octaves = octaves,
      .frequencies = {},
      .amplitudes = {},
      .periods = {},
      .offsets = {}};

  float amplitude = 1.0f;
  float amplitude_sum = 0.0f;
  float scale = 1.0f;
  for (int octave = 0; octave < octaves; ++octave) {
    if (periodic) {
      // glm's periodic perlin only wraps without a seam on whole cells, so
      // round this octave's cell count, then fit its frequency so those cells
      // span period * feature_size, the same distance for every octave
      const float cells = std::max(1.0f, std::round(period * scale));
      displacement.periods[octave] = cells;
      displacement.frequencies[octave] = cells / (period * feature_size);
    } else {
      displacement.frequencies[octave] = scale / feature_size;
    }
    displacement.amplitudes[octave] = amplitude;
    amplitude_sum += amplitude;
    amplitude *= decay;
    scale *= lacunarity;
  }
  for (int octave = 0; octave < octaves; ++octave) {
    displacement.amplitudes[octave] /= amplitude_sum;
  }

  // raw engine output is the same on every platform, unlike std distributions,
  // and every octave is drawn so changing the octave count reshuffles nothing
  std::mt19937 engine(static_cast<std::mt19937::result_type>(config.seed));
  const auto next_offset = [&engine] {
    // glm's structure wraps at 289 cells, idk why...
    constexpr float field_period = 289.0f;
    return static_cast<float>(engine() >> 8) * (field_period / 16777216.0f);
  };
  for (auto &octave_offsets : displacement.offsets) {
    for (auto &axis_offset : octave_offsets) {
      axis_offset.x = next_offset();
      axis_offset.y = next_offset();
      axis_offset.z = next_offset();
    }
  }

  return displacement;
}

} // namespace

void NoiseOperator::init(OperatorHost *host,
                         Corrade::PluginManager::Manager<backend::BackendPlugin>
                             &backend_plugin_manager) {
  OperatorPlugin::init(host, backend_plugin_manager);
  pc::logger()->trace("Initialised NoiseOperator");
}

PipelineFramePtr NoiseOperator::process(PipelineFramePtr input) {
  using profiling::ProfilingZone;

  auto variant = load_config();
  if (!variant) return input;
  const auto &config = std::get<NoiseConfiguration>(*variant);

  if (!input->cloud || input->cloud->empty()) return input;
  if (config.magnitude.mm == 0) return input;

  ProfilingZone operator_zone("NoiseOperator");
  operator_zone.text(config.id);

  const auto displacement = displacement_parameters(config);

  auto displaced_cloud = std::make_shared<PointCloud>();

  if (current_backend_type() == BackendType::CUDA) {
    try {
      _cuda_noise.displace_cloud(*input->cloud, *displaced_cloud, displacement);
    } catch (const std::exception &e) {
      pc::logger()->error("NoiseOperator CUDA error: {}", e.what());
      return input;
    }
  } else {
    noise::displace_cloud_cpu(*input->cloud, *displaced_cloud, displacement);
  }

  auto output_frame = input->clone();
  output_frame->cloud = std::move(displaced_cloud);
  return output_frame;
}

} // namespace pc::operators

CORRADE_PLUGIN_REGISTER(NoiseOperator, pc::operators::NoiseOperator,
                        "net.pointcaster.OperatorPlugin/1.0")
