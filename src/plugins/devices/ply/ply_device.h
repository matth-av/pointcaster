#pragma once

#include "../device_plugin.h"
#include "plugins/devices/device_status.h"
#include "ply_device_config.h"
#include "ply_sequence_loader.h"

#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/Pointer.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <atomic>
#include <chrono>
#include <plugins/backend/backend_plugin.h>
#include <readerwriterqueue/readerwritercircularbuffer.h>
#include <string_view>
#include <thread>
#include <vector>

namespace pc::devices {

class PlyDevice : public DevicePlugin {
public:
  explicit PlyDevice(Corrade::PluginManager::AbstractManager &manager,
                     Corrade::Containers::StringView plugin)
      : DevicePlugin(manager, plugin) {}

  ~PlyDevice() override;

  PlyDevice(const PlyDevice &) = delete;
  PlyDevice &operator=(const PlyDevice &) = delete;
  PlyDevice(PlyDevice &&) = delete;
  PlyDevice &operator=(PlyDevice &&) = delete;

  DeviceStatus status() const override { return _status; };

  std::shared_ptr<PointCloud> point_cloud() override;

  void start() override {};
  void stop() override {};
  void restart() override {};

  void reprocess() override {
    std::lock_guard lock(_device_mutex);
    apply_transform();
  }

  void on_pipeline_output(operators::PipelineFramePtr output_frame) override;

  void on_config_field_changed(std::string_view path = "") override;

  void
  update_config(const devices::DeviceConfigurationVariant &config) override;

  bool load(std::string_view url);
  void reload();

  void on_session_membership_changed(bool in_any_session) override;

  void tick(float delta_time);
  bool is_sequence() const override { return _sequence_loader != nullptr; }
  size_t frame_count() const override;

private:
  DeviceStatus _status = DeviceStatus::Unloaded;

  std::string _loaded_file_path{};
  std::shared_ptr<ply::PlySequenceLoader> _sequence_loader;
  float _frame_accumulator = 0.f;
  int _current_frame = 0;

  std::shared_ptr<PointCloud> _input_cloud;

  std::atomic<std::shared_ptr<PointCloud>> _current_point_cloud{
      std::make_shared<PointCloud>(PointCloud{{}, {}, {}})};

  // TODO change this to a device-global or workspace-global timer thread
  std::jthread _tick_thread;
  std::mutex _device_mutex;

  bool load_directory(const std::filesystem::path &dir);

  void reload_current_frame();

  void apply_transform();
};

} // namespace pc::devices