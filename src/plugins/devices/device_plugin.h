#pragma once

#include "device_status.h"
#include "device_tree.h"
#include "device_variants.h"
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Pointer.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <Corrade/Tags.h>
#include <atomic>
#include <cpplocate/cpplocate.h>
#include <filesystem>
#include <functional>
#include <logger/logger.h>
#include <memory>
#include <metrics/metrics.h>
#include <mutex>
#include <pipeline/concurrent_operator_pipeline.h>
#include <pipeline/pipeline_frame.h>
#include <plugins/backend/backend_plugin.h>
#include <plugins/backend/backend_set.h>
#include <plugins/backend/backend_types.h>
#include <plugins/operators/operator_host.h>
#include <plugins/operators/operator_plugin.h>
#include <plugins/operators/operator_variants.h>
#include <pointcaster/point_cloud.h>
#include <pointcaster_api.h>
#include <span>
#include <string>
#include <string_view>
#include <typeinfo>
#include <variant>

namespace pc {
class Workspace;
}

namespace pc::devices {

struct DiscoveredDevice {
  std::string label;
  std::string ip;
  std::string id;
  std::string type_label;
};

struct PluginSettingsPage {
  std::string key;
  std::string title;
  std::string qml_file_path;
};

// what a device's source declared about one of its per-point attributes
struct ImportedAttribute {
  std::string name;
  std::string type_name;
};

struct ImportedAttributes {
  std::vector<ImportedAttribute> attributes;
};

class POINTCASTER_CLASS_API DevicePlugin
    : public Corrade::PluginManager::AbstractPlugin,
      public pc::operators::OperatorHost {
public:
  static Corrade::Containers::StringView pluginInterface() {
    using namespace Corrade::Containers::Literals;
    return "net.pointcaster.DevicePlugin/1.0"_s;
  }

  static Corrade::Containers::Array<Corrade::Containers::String>
  pluginSearchPaths() {
    std::filesystem::path exe_dir(cpplocate::getModulePath());
#ifdef _WIN32
    // flat windows layout: plugins/ sits beside the executable
    auto plugin_dir = exe_dir / "plugins" / "devices";
#else
    // linux bin/ layout: plugins/ sits beside the executable's parent dir
    auto plugin_dir = exe_dir.parent_path() / "plugins" / "devices";
#endif

    std::string plugin_path = plugin_dir.string();

#ifdef _WIN32
    // convert C:\style\path into posix /style/path used by corrade
    plugin_path.erase(0, 2);
    std::replace(plugin_path.begin(), plugin_path.end(), '\\', '/');
#endif

    // return results in a corrade array (required instead of vector)
    static Corrade::Containers::Array<Corrade::Containers::String> results;
    arrayClear(results);
    arrayAppend(results, Corrade::Containers::String{plugin_path.c_str()});

    return {Corrade::InPlaceInit, results};
  }

  explicit DevicePlugin(Corrade::PluginManager::AbstractManager &manager,
                        Corrade::Containers::StringView plugin)
      : Corrade::PluginManager::AbstractPlugin{manager, plugin} {
    static bool loaded_discovery_instance = false;
    if (!loaded_discovery_instance) {
      _is_discovery_instance = true;
      loaded_discovery_instance = true;
    }
  }

  virtual ~DevicePlugin() = default;

  POINTCASTER_API void init(Workspace &workspace);

  void set_is_discovery_instance(bool v) { _is_discovery_instance = v; }
  bool is_discovery_instance() { return _is_discovery_instance; };

  virtual std::vector<DiscoveredDevice> discovered_devices() const {
    return {};
  };
  virtual void refresh_discovery() {};

  virtual std::vector<PluginSettingsPage> settings_pages() const { return {}; }

  // directory holding this plugin's own binary; the derived type's rtti lives
  // in whichever binary defines it, so locating that locates the plugin
  std::filesystem::path plugin_directory() const {
    auto *plugin_type_info =
        const_cast<void *>(static_cast<const void *>(&typeid(*this)));
    return std::filesystem::path(cpplocate::getLibraryPath(plugin_type_info))
        .parent_path();
  }

  bool in_any_session() const {
    return _in_any_session.load(std::memory_order_acquire);
  }
  void set_in_any_session(bool value) {
    _in_any_session.store(value, std::memory_order_release);
  }

  virtual void on_session_membership_changed(bool /*in_any_session*/) {}

  POINTCASTER_API BackendType current_backend_type() const;
  POINTCASTER_API backend::BackendPlugin *current_backend() const;
  POINTCASTER_API backend::BackendPlugin *backend_for(BackendType) const;

  // the cpu backend is always available
  POINTCASTER_API backend::BackendPlugin *cpu_backend() const;

  virtual void add_discovery_change_callback(std::function<void()>){};
  virtual bool has_discovery_change_callback() const { return false; };

  virtual DeviceStatus status() const = 0;

  void set_status_callback(std::function<void(DeviceStatus)> cb) {
    std::scoped_lock lock(_callback_access);
    _status_callback = std::move(cb);
  }

  void set_point_cloud_updated_callback(std::function<void()> cb) {
    std::scoped_lock lock(_callback_access);
    _point_cloud_updated_callback = std::move(cb);
  }

  void detach_callbacks() {
    _callbacks_detached.store(true, std::memory_order_release);
    std::scoped_lock lock(_callback_access);
    _status_callback = nullptr;
    _point_cloud_updated_callback = nullptr;
  }

  bool callbacks_detached() const {
    return _callbacks_detached.load(std::memory_order_acquire);
  }

  DeviceConfigurationVariant &config() { return _config; }

  POINTCASTER_API virtual void
  update_config(const DeviceConfigurationVariant &config);

  POINTCASTER_API virtual void
  on_config_field_changed([[maybe_unused]] std::string_view path = "");

  virtual bool plugin_null_state() const { return false; }

  virtual std::shared_ptr<PointCloud> point_cloud() = 0;

  std::shared_ptr<std::vector<std::byte>> render_data() {
    return _latest_render_data.load(std::memory_order_acquire);
  }

  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void restart() = 0;

  virtual void shutdown() {}

  void reprocess() override {}

  virtual bool is_sequence() const { return false; }
  virtual size_t frame_count() const { return 1; }

  // a device that imports named per-point attributes (like pscale)
  // must declare them here
  virtual ImportedAttributes imported_attributes() const { return {}; }

  void notify_status_changed(DeviceStatus new_status) {
    if (callbacks_detached()) return;
    std::scoped_lock lock(_callback_access);
    if (_status_callback) _status_callback(new_status);
  }
  void notify_status_changed() { notify_status_changed(status()); }

  void notify_point_cloud_updated() {
    if (callbacks_detached()) return;
    std::scoped_lock lock(_callback_access);
    if (_point_cloud_updated_callback) _point_cloud_updated_callback();
  }

  DeviceConfigurationVariant &config_variant() { return _config; }

  static constexpr size_t max_frame_tasks_in_flight = 2;

  // claims a processing slot, or returns false when the device is already at
  // capacity and this frame should be dropped
  bool try_begin_frame_task() {
    auto in_flight = _process_tasks_in_flight.load(std::memory_order_relaxed);
    while (in_flight < max_frame_tasks_in_flight) {
      // on failure the exchange refreshes in_flight, so the loop re-checks
      // capacity against the value another thread just left behind
      if (_process_tasks_in_flight.compare_exchange_weak(
              in_flight, in_flight + 1, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return true;
      }
    }
    const auto dropped_frames_previous =
        _dropped_frames.fetch_add(1, std::memory_order_relaxed);

    pc::metrics::set_gauge("pointcaster_device_frames_dropped",
                           dropped_frames_previous + 1,
                           {{"device_id", device_id_from_variant(_config)}});
    return false;
  }

  // releases a slot claimed by try_begin_frame_task
  void end_frame_task() {
    _frames_processed.fetch_add(1, std::memory_order_relaxed);
    _process_tasks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    _process_tasks_in_flight.notify_all();
  }

  class FrameTaskSlot {
  public:
    explicit FrameTaskSlot(DevicePlugin &device) : _device(device) {}
    ~FrameTaskSlot() { _device.end_frame_task(); }

    FrameTaskSlot(const FrameTaskSlot &) = delete;
    FrameTaskSlot &operator=(const FrameTaskSlot &) = delete;
    FrameTaskSlot(FrameTaskSlot &&) = delete;
    FrameTaskSlot &operator=(FrameTaskSlot &&) = delete;

  private:
    DevicePlugin &_device;
  };

  size_t frame_tasks_in_flight() const {
    return _process_tasks_in_flight.load(std::memory_order_relaxed);
  }

  size_t dropped_frames() const {
    return _dropped_frames.load(std::memory_order_relaxed);
  }

  size_t frames_processed() const {
    return _frames_processed.load(std::memory_order_relaxed);
  }

  std::vector<camera::CameraFrame> latest_camera_frames() const {
    if (_pipeline) return _pipeline->latest_camera_frames();
    return {};
  }

  operators::ConcurrentOperatorPipelineConfiguration &
  pipeline_config() override {
    return std::visit(
        [](auto &device_config)
            -> operators::ConcurrentOperatorPipelineConfiguration & {
          if constexpr (requires { device_config.operator_pipeline; }) {
            return device_config.operator_pipeline.value();
          } else {
            // TODO
            // groups have no operator pipeline yet...
            static operators::ConcurrentOperatorPipelineConfiguration fallback;
            return fallback;
          }
        },
        _config);
  }

protected:
  DeviceConfigurationVariant _config;

  backend::BackendSet _backends;
  std::mutex _callback_access;
  std::function<void(DeviceStatus)> _status_callback;
  std::function<void()> _point_cloud_updated_callback;
  std::atomic_bool _callbacks_detached{false};
  std::atomic_bool _in_any_session{true};
  bool _is_discovery_instance = false;

  std::atomic<size_t> _process_tasks_in_flight{0};
  std::atomic<size_t> _dropped_frames{0};
  std::atomic<size_t> _frames_processed{0};

  std::atomic<std::shared_ptr<std::vector<std::byte>>> _latest_render_data;

  void on_pipeline_output(operators::PipelineFramePtr) override {
    notify_point_cloud_updated();
  }

private:
  virtual void init() {};
};

} // namespace pc::devices
