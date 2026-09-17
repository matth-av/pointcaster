#pragma once

#include "operator_host.h"
#include "operator_variants.h"

#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringStlView.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/Tags.h>
#include <camera/camera_frame.h>
#include <cpplocate/cpplocate.h>
#include <filesystem>
#include <logger/logger.h>
#include <pipeline/concurrent_container.h>
#include <pipeline/pipeline_frame.h>
#include <plugins/backend/backend_plugin.h>
#include <plugins/backend/backend_set.h>
#include <plugins/backend/backend_types.h>
#include <pointcaster/point_cloud.h>
#include <span>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pc {
class Workspace;
}

namespace pc::operators {

class OperatorPlugin : public Corrade::PluginManager::AbstractPlugin {
public:
  static Corrade::Containers::StringView pluginInterface() {
    using namespace Corrade::Containers::Literals;
    return "net.pointcaster.OperatorPlugin/1.0"_s;
  }

  static Corrade::Containers::Array<Corrade::Containers::String>
  pluginSearchPaths() {
    std::filesystem::path exe_dir(cpplocate::getModulePath());
#ifdef _WIN32
    // flat windows layout: plugins/ sits beside the executable
    auto plugin_dir = exe_dir / "plugins" / "operators";
#else
    // linux bin/ layout: plugins/ sits beside the executable's parent dir
    auto plugin_dir = exe_dir.parent_path() / "plugins" / "operators";
#endif

    std::vector<Corrade::Containers::String> search_paths;

    if (std::filesystem::exists(plugin_dir)) {
      for (auto &recursive_file_node :
           std::filesystem::recursive_directory_iterator(plugin_dir)) {
        if (recursive_file_node.path().extension().string() == ".conf") {
          // for each subdirectory that contains a file named .conf,
          // treat it as a plugin directory and add it to our search paths...
          search_paths.emplace_back(
              recursive_file_node.path().parent_path().string());
        }
      }
    }

    // corrade requires at least one search path entry even when no plugins
    // exist
    if (search_paths.empty()) {
      search_paths.emplace_back(plugin_dir.string());
    }

#ifdef _WIN32
    // convert C:\style\path into posix /style/path used by corrade
    for (auto &path_entry : search_paths) {
      std::string result(path_entry.data());
      result.erase(0, 2);
      std::replace(result.begin(), result.end(), '\\', '/');
      path_entry = result;
    }
#endif

    // return results in a corrade array (required instead of vector)
    static Corrade::Containers::Array<Corrade::Containers::String> results;
    arrayClear(results);
    arrayReserve(results, search_paths.size());
    for (auto &path_string : search_paths) {
      arrayAppend(results, path_string);
    }

    return {Corrade::InPlaceInit, results};
  }

  explicit OperatorPlugin(Corrade::PluginManager::AbstractManager &manager,
                          Corrade::Containers::StringView plugin)
      : Corrade::PluginManager::AbstractPlugin{manager, plugin} {
    pc::logger()->debug("Operator plugin constructor");
  }

  virtual ~OperatorPlugin() = default;

  virtual void init(OperatorHost *host,
                    Corrade::PluginManager::Manager<backend::BackendPlugin>
                        &backend_plugin_manager) {
    _host = host;

    const std::string operator_name{plugin()};

    pc::logger()->trace("Initialising operator plugin: {}", operator_name);

    // initialise backends depending on what plugins are available
    _backends.instantiate(backend_plugin_manager, operator_name);

    std::visit([this](const auto &c) { set_current_backend(c.backend); },
               config_variant());
  };

  // return 'input' unchanged to pass a frame straight through...
  // to change the frame, take input->clone(), modify that, and return it
  virtual PipelineFramePtr process(PipelineFramePtr input) = 0;

  void update_config(const OperatorConfigurationVariant &config) {
    _config.store(std::make_shared<const OperatorConfigurationVariant>(config));
  }

  const OperatorConfigurationVariant &config_variant() const {
    return *load_config();
  }

  // if on_config_field_changed needs to be overriden, make sure to call
  // OperatorPlugin::on_config_field_changed as well...
  virtual void on_config_field_changed(std::string_view path = "") {
    if (path == "backend") {
      std::visit(
          [this](const auto &config) { set_current_backend(config.backend); },
          config_variant());
    }
  }

  virtual bool plugin_null_state() const { return false; }

  // if an operator projects any camera / intermediary views into 2d that should
  // be displayed in the UI, declare them as member variables and return them
  // here
  virtual std::vector<camera::CameraFrameRef> camera_frames() { return {}; }

  // switches to a backend and reports the one actually in use
  BackendType set_current_backend(BackendType requested_backend) {
    const auto resolved_backend = _backends.resolve(requested_backend);
    if (!resolved_backend) {
      pc::logger()->error("{} has no backends loaded",
                          std::string_view{plugin()});
      _current_backend = nullptr;
      return _current_backend_type;
    }

    if (*resolved_backend != requested_backend) {
      pc::logger()->warn("{} backend is not loaded, using {}",
                         backend_name(requested_backend),
                         backend_name(*resolved_backend));
    }

    _current_backend = _backends.get(*resolved_backend);
    _current_backend_type = *resolved_backend;
    return _current_backend_type;
  }

  BackendType current_backend_type() const { return _current_backend_type; }

  bool has_backend() const { return _current_backend != nullptr; }

  // the cpu backend is always available, whatever the operator is set to
  backend::BackendPlugin *cpu_backend() const { return _backends.cpu(); }

  static constexpr std::string_view backend_name(BackendType backend_type) {
    return backend::backend_name(backend_type);
  }

protected:
  OperatorHost *_host = nullptr;

  ConcurrentContainer<OperatorConfigurationVariant> _config;

  std::shared_ptr<const OperatorConfigurationVariant> load_config() const {
    return _config.load();
  }

  backend::BackendSet _backends;

  backend::BackendPlugin *_current_backend = nullptr;
  BackendType _current_backend_type = BackendType::CPU;
};

} // namespace pc::operators
