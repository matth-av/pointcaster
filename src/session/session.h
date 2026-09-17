#pragma once

#include "session_config.h"

#include <Corrade/Containers/Pointer.h>
#include <atomic>
#include <camera/camera_frame.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <logger/logger.h>
#include <memory>
#include <mutex>
#include <plugins/backend/backend_plugin.h>
#include <plugins/backend/backend_set.h>
#include <plugins/operators/operator_host.h>
#include <pointcaster/point_cloud.h>
#include <thread>

namespace pc {

class Workspace;
struct WorkspaceConfiguration;

namespace operators {
class OperatorPlugin;
}

namespace devices {
class DevicePlugin;
}

class Session : public operators::OperatorHost {
public:
  const std::string id;

  explicit Session(Workspace &workspace, const SessionConfiguration &config);

  void reprocess() override {
    _force_reprocess.store(true, std::memory_order_release);
  }

  void update_config(const SessionConfiguration &config);

  // ---- point cloud surface (Qt bridge reads these) ----
  void set_point_cloud_updated_callback(std::function<void()> cb) {
    _point_cloud_updated_callback = std::move(cb);
  }
  std::shared_ptr<PointCloud> point_cloud() const {
    return _current_point_cloud.load(std::memory_order_acquire);
  }
  std::shared_ptr<std::vector<std::byte>> render_data() const {
    return _latest_render_data.load(std::memory_order_acquire);
  }

  std::vector<camera::CameraFrame> latest_camera_frames() const;

  // ---- session-wide sequence playback (master clock on the update thread)
  void playback_play();
  void playback_pause();
  void playback_stop();
  void playback_scrub(int frame);
  void playback_set_loop(int in, int out); // out < 0 == end of timeline
  void playback_set_looping(bool looping);
  void playback_set_fps(int fps);

  bool playback_has_sequence() const {
    return _cached_has_sequence.load(std::memory_order_acquire);
  }

  int playback_total_frames() const {
    return _cached_session_length.load(std::memory_order_acquire);
  }

  bool playback_is_playing() const {
    std::lock_guard lk(_playback_mutex);
    return _playback.playing;
  }

  int playback_current_frame() const {
    std::lock_guard lk(_playback_mutex);
    return _playback.current_frame;
  }

  int playback_loop_in() const {
    std::lock_guard lk(_playback_mutex);
    return _playback.start_frame;
  }

  int playback_loop_out() const {
    int end;
    {
      std::lock_guard lk(_playback_mutex);
      end = _playback.end_frame;
    }
    return end >= 0 ? end
                    : std::max(0, _cached_session_length.load(
                                      std::memory_order_acquire) -
                                      1);
  }

  bool playback_looping() const {
    std::lock_guard lk(_playback_mutex);
    return _playback.looping;
  }

  // Fired (off the UI thread) whenever frame / play-state / loop / total
  // change.
  void set_playback_changed_callback(std::function<void()> cb) {
    _playback_changed_callback = std::move(cb);
  }

protected:
  void on_pipeline_output(operators::PipelineFramePtr output_frame) override;

  operators::ConcurrentOperatorPipelineConfiguration &
  pipeline_config() override {
    return _config.operator_pipeline;
  }

private:
  SessionConfiguration _config;
  std::jthread _update_thread;

  backend::BackendSet _backends;

  std::atomic<std::shared_ptr<PointCloud>> _current_point_cloud{nullptr};
  std::atomic<std::shared_ptr<std::vector<std::byte>>> _latest_render_data{
      nullptr};
  std::function<void()> _point_cloud_updated_callback;

  std::atomic<bool> _force_reprocess{false};

  // ---- playback state ----
  struct Playback {
    bool playing = false;
    int current_frame = 0;
    int start_frame = 0;
    int end_frame = -1; // < 0 == end of timeline
    bool looping = true;
    int fps = 30;
    int explicit_length = -1;
  };
  Playback _playback;
  mutable std::mutex _playback_mutex;
  double _playback_accum = 0.0;

  std::function<void()> _playback_changed_callback;

  std::atomic<int> _cached_total_frames{1};
  std::atomic<int> _cached_session_length{1};
  std::atomic<bool> _cached_has_sequence{false};

  void notify_point_cloud_updated() {
    if (_point_cloud_updated_callback) _point_cloud_updated_callback();
  }
  void notify_playback_changed() {
    if (_playback_changed_callback) _playback_changed_callback();
  }

  static bool includes_device(const WorkspaceConfiguration &workspace_config,
                              const SessionConfiguration &session_config,
                              devices::DevicePlugin &device);
  bool includes_device(devices::DevicePlugin &device) const;

  void update_loop(std::stop_token stop_token);
  void advance_playback(double dt_seconds);
  void scrub_devices(int session_frame);
  int resolve_length(int total_device_frames) const;
};

} // namespace pc