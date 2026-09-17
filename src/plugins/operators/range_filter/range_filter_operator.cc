#include "range_filter_operator.h"

#include <algorithm>
#include <memory>
#include <profiling/profiling_zone.h>

namespace pc::operators {

namespace {} // namespace

void RangeFilterOperator::init(
    OperatorHost *host, Corrade::PluginManager::Manager<backend::BackendPlugin>
                            &backend_plugin_manager) {
  OperatorPlugin::init(host, backend_plugin_manager);
  pc::logger()->trace("Initialised RangeFilterOperator");
}

PipelineFramePtr RangeFilterOperator::process(PipelineFramePtr input) {
  using profiling::ProfilingZone;

  auto variant = load_config();
  if (!variant) return input;
  const auto &config = std::get<RangeFilterConfiguration>(*variant);

  ProfilingZone operator_zone("RangeFilterOperator");
  operator_zone.text(config.id);

  auto clear_outputs = [&](const auto point_count) {
    config.point_count.set(point_count);
    config.fill_value.set(0.0f);
    config.proportion.set(0.0f);
    config.occupied_bounds.set({{}, {}});
  };

  if (!input->cloud || input->cloud->empty()) {
    clear_outputs(0);
    return input;
  }

  const auto &bounds = config.bounds;
  const bool invert = config.invert;
  const bool bypass = config.bypass;

  PointCloud empty;
  auto filtered_cloud = bypass ? nullptr : std::make_shared<PointCloud>();

  const auto result = _current_backend->filter_to_bounds(
      *input->cloud, bypass ? empty : *filtered_cloud, bounds,
      {.invert = invert, .analyse_only = bypass});

  const auto point_count = static_cast<int>(result.point_count);

  if (point_count <= config.count_threshold) {
    clear_outputs(point_count);
    if (bypass) return input;
    auto output_frame = input->clone();
    output_frame->cloud = std::move(filtered_cloud);
    return output_frame;
  }

  {
    ProfilingZone output_zone("RangeFilterOperator::outputs");

    const auto count = static_cast<float>(result.point_count);
    const auto max_fill = static_cast<float>(std::max(1, config.max_fill));
    const auto input_count =
        static_cast<float>(std::max<size_t>(1, result.input_count));

    config.point_count.set(point_count);
    config.fill_value.set(count / max_fill);
    config.proportion.set(count / input_count);

    // occupied bounds are invalid when the range is inverted
    static constexpr position_bounds empty{{}, {}};
    config.occupied_bounds.set(invert ? empty : result.bounds);
  }

  if (bypass) return input;

  auto output_frame = input->clone();
  output_frame->cloud = std::move(filtered_cloud);
  return output_frame;
}

} // namespace pc::operators

CORRADE_PLUGIN_REGISTER(RangeFilterOperator, pc::operators::RangeFilterOperator,
                        "net.pointcaster.OperatorPlugin/1.0")
