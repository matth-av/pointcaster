#include "operator_host.h"

#include <logger/logger.h>
#include <mutex>
#include <pipeline/concurrent_operator_pipeline.h>
#include <pointcaster/plugin_manager_lock.h>
#include <plugins/operators/operator_plugin.h>
#include <plugins/operators/operator_variants.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <workspace/workspace.h>

namespace pc::operators {

OperatorHost::~OperatorHost() = default;

void OperatorHost::sync_operators(
    std::span<const OperatorConfigurationVariant> configs) {

  if (!_workspace) {
    pc::logger()->error("sync_operators: _workspace null, this={}",
                        fmt::ptr(this));
    assert(_workspace &&
           "OperatorHost::init(workspace) never ran on this instance");
    return;
  }

  std::unordered_map<std::string, std::size_t> existing_index_by_id;
  existing_index_by_id.reserve(operators.size());
  for (std::size_t i = 0; i < operators.size(); ++i) {
    if (!operators[i]) continue;
    const auto [id, _] =
        operator_info_from_variant(operators[i]->config_variant());
    if (id.empty()) continue;
    existing_index_by_id.try_emplace(id, i);
  }

  std::vector<Corrade::Containers::Pointer<OperatorPlugin>> new_operators;
  new_operators.reserve(configs.size());

  for (const auto &operator_variant : configs) {
    auto [operator_id, operator_plugin_name] =
        operator_info_from_variant(operator_variant);
    if (operator_id.empty()) {
      pc::logger()->warn("Operator config missing id; skipping");
      continue;
    }

    Corrade::Containers::Pointer<OperatorPlugin> op_plugin;
    bool new_instance = false;

    if (!(_workspace->operator_plugin_manager->loadState(
              std::string(operator_plugin_name)) &
          Corrade::PluginManager::LoadState::Loaded)) {
      pc::logger()->error(
          "Operator plugin '{}' is not loaded; skipping operator id='{}'",
          operator_plugin_name, operator_id);
      continue;
    }

    auto it = existing_index_by_id.find(operator_id);
    if (it != existing_index_by_id.end()) {
      const auto idx = it->second;
      existing_index_by_id.erase(it);

      auto &existing_ptr = operators[idx];
      const auto [_, existing_plugin_name] =
          operator_info_from_variant(existing_ptr->config_variant());

      if (existing_plugin_name == operator_plugin_name) {
        existing_ptr->update_config(operator_variant);
        op_plugin = std::move(existing_ptr);
      } else {
        pc::logger()->info("Operator id='{}' type '{}' -> '{}'", operator_id,
                           existing_plugin_name, operator_plugin_name);
        std::scoped_lock plugin_lock(pc::plugin_manager_access());
        existing_ptr = nullptr;
        op_plugin = _workspace->operator_plugin_manager->instantiate(
            std::string(operator_plugin_name));
        new_instance = true;
      }
    } else {
      std::scoped_lock plugin_lock(pc::plugin_manager_access());
      op_plugin = _workspace->operator_plugin_manager->instantiate(
          std::string(operator_plugin_name));
      new_instance = true;
    }

    op_plugin->update_config(operator_variant);
    if (new_instance) {
      op_plugin->init(this, *_workspace->backend_plugin_manager);
    }

    new_operators.push_back(std::move(op_plugin));
  }

  bool changed = (new_operators.size() != operators.size());
  if (!changed) {
    for (size_t i = 0; i < new_operators.size(); ++i) {
      if (new_operators[i].get() != operators[i].get()) {
        changed = true;
        break;
      }
    }
  }

  // remaining entries in existing_index_by_id are deletions; just drop
  {
    std::scoped_lock plugin_lock(pc::plugin_manager_access());
    operators = std::move(new_operators);
  }

  bool need_rebuild = changed;
  {
    std::scoped_lock lock(_pipeline_mutex);
    if (!_pipeline ||
        _pipeline->worker_chains().size() != pipeline_concurrency()) {
      need_rebuild = true;
    }
  }
  if (need_rebuild) {
    rebuild_pipeline(configs);
  }
}

void OperatorHost::rebuild_pipeline(
    std::span<const OperatorConfigurationVariant> configs) {
  // build the chains outside the lock; they don't touch _pipeline
  std::vector<operators::OperatorPipelineWorkerChain> pipeline_worker_chains =
      operators::build_worker_chains(configs, pipeline_concurrency(), *this,
                                     *_workspace);

  std::unique_ptr<operators::ConcurrentOperatorPipeline> old_pipeline;
  {
    std::scoped_lock lock(_pipeline_mutex);
    old_pipeline = std::move(_pipeline);
  }
  if (old_pipeline) {
    old_pipeline->stop();
    std::scoped_lock plugin_lock(pc::plugin_manager_access());
    old_pipeline.reset();
  }

  auto new_pipeline = std::make_unique<operators::ConcurrentOperatorPipeline>(
      std::move(pipeline_worker_chains));

  new_pipeline->set_on_complete([this](PipelineFramePtr output_frame) {
    on_pipeline_output(std::move(output_frame));
  });

  {
    std::scoped_lock lock(_pipeline_mutex);
    _pipeline = std::move(new_pipeline);
    _pipeline->start();
  }
}

void OperatorHost::feed_operator_pipeline(std::shared_ptr<PointCloud> cloud) {
  std::scoped_lock lock(_pipeline_mutex);
  if (_pipeline) {
    bool success = _pipeline->submit(cloud);
    if (!success) {
      pc::logger()->error(
          "Failed to submit device pointcloud into operator pipeline");
    }
  }
}

void OperatorHost::update_operator_in_pipeline(
    const OperatorConfigurationVariant &config, std::string_view changed_path) {
  std::scoped_lock lock(_pipeline_mutex);
  if (!_pipeline) return;
  auto [target_id, _] = operator_info_from_variant(config);
  for (auto &chain : _pipeline->worker_chains()) {
    for (auto &op : chain) {
      auto [id, __] = operator_info_from_variant(op->config_variant());
      if (id == target_id) {
        op->update_config(config);
        op->on_config_field_changed(changed_path);
      }
    }
  }
}

} // namespace pc::operators