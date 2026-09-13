#include "session.h"
#include "session/session_config.h"

#include <algorithm>
#include <logger/logger.h>
#include <memory>
#include <pipeline/concurrent_operator_pipeline.h>
#include <plugins/devices/device_plugin.h>
#include <plugins/devices/device_tree.h>
#include <stop_token>
#include <workspace/workspace.h>

namespace pc {

Session::Session(Workspace &workspace, const SessionConfiguration &config)
    : id(config.id) {
  _workspace = &workspace;

  if (_workspace->backend_plugin_manager) {
    _backends.instantiate(*_workspace->backend_plugin_manager, "Session");
  }

  update_config(config);

  _update_thread = std::jthread(
      [this](std::stop_token stop) { update_loop(std::move(stop)); });
}

void Session::update_config(const SessionConfiguration &config) {
  const int prev_config_frame = _config.timeline.value().current_frame.value();
  _config = config;
  sync_operators(_config.operators);

  {
    const auto &tl = _config.timeline.value();
    std::lock_guard lk(_playback_mutex);
    _playback.looping = tl.looping.value();
    _playback.fps = std::max(1, tl.fps.value());
    _playback.start_frame = std::max(0, tl.start_frame.value());
    _playback.end_frame = tl.end_frame.value();
    _playback.explicit_length = tl.length.value();
    const int new_config_frame = tl.current_frame.value();
    if (new_config_frame != prev_config_frame) {
      _playback.current_frame =
          std::max(_playback.start_frame, new_config_frame);
    } else {
      _playback.current_frame =
          std::max(_playback.start_frame, _playback.current_frame);
    }
  }

  _force_reprocess.store(true, std::memory_order_release);
}

bool Session::includes_device(const WorkspaceConfiguration &workspace_config,
                              const SessionConfiguration &session_config,
                              devices::DevicePlugin &device) {
  return devices::effective_session_enabled(
      workspace_config, session_config,
      std::string(devices::device_id_from_variant(device.config())));
}

bool Session::includes_device(devices::DevicePlugin &device) const {
  const auto &session_configs = _workspace->config.sessions;
  auto session_config =
      std::ranges::find(session_configs, id, &SessionConfiguration::id);
  if (session_config == session_configs.end()) return false;
  return includes_device(_workspace->config, *session_config, device);
}

std::vector<camera::CameraFrame> Session::latest_camera_frames() const {
  if (_pipeline) return _pipeline->latest_camera_frames();
  return {};
}

void Session::on_pipeline_output(operators::PipelineFramePtr output_frame) {
  if (!output_frame) return;
  auto cloud = output_frame->cloud;
  if (auto *cpu = _backends.cpu(); cloud && !cloud->empty() && cpu) {
    auto buf = std::make_shared<std::vector<std::byte>>(cloud->size() * 16);
    cpu->pack_render_buffer(*cloud, *buf);
    _latest_render_data.store(std::move(buf), std::memory_order_release);
  }
  _current_point_cloud.store(cloud, std::memory_order_release);
  notify_point_cloud_updated();
}

// ---------------- playback control (called from the UI thread)
// ----------------

void Session::playback_play() {
  int frame;
  {
    std::lock_guard lk(_playback_mutex);
    _playback.playing = true;
    _playback_accum = 0.0;
    frame = _playback.current_frame;
  }
  scrub_devices(
      frame); // snap devices to current; also pauses their self-advance
  notify_playback_changed();
}

void Session::playback_pause() {
  {
    std::lock_guard lk(_playback_mutex);
    _playback.playing = false;
  }
  notify_playback_changed();
}

void Session::playback_stop() {
  int frame;
  {
    std::lock_guard lk(_playback_mutex);
    _playback.playing = false;
    _playback_accum = 0.0;
    _playback.current_frame = std::max(0, _playback.start_frame);
    frame = _playback.current_frame;
  }
  scrub_devices(frame);
  notify_playback_changed();
}

void Session::playback_scrub(int frame) {
  int f;
  {
    std::lock_guard lk(_playback_mutex);
    f = std::max(_playback.start_frame, frame);
    _playback.current_frame = f;
  }
  scrub_devices(f);
  notify_playback_changed();
}

void Session::playback_set_loop(int in, int out) {
  {
    std::lock_guard lk(_playback_mutex);
    _playback.start_frame = std::max(0, in);
    _playback.end_frame = out; // <0 means "end of timeline"
  }
  notify_playback_changed();
}

void Session::playback_set_looping(bool looping) {
  {
    std::lock_guard lk(_playback_mutex);
    _playback.looping = looping;
  }
  notify_playback_changed();
}

void Session::playback_set_fps(int fps) {
  {
    std::lock_guard lk(_playback_mutex);
    _playback.fps = std::max(1, fps);
  }
  notify_playback_changed();
}

// ---------------- device scrubbing ----------------

void Session::scrub_devices(int session_frame) {
  std::vector<devices::DevicePlugin *> device_ptrs;
  {
    std::lock_guard lock(_workspace->config_access);
    for (auto &device : _workspace->devices) {
      if (!device || device->is_discovery_instance()) continue;
      if (!includes_device(*device)) continue;
      device_ptrs.push_back(device.get());
    }
  }

  // Use the absolute session frame as the offset into each device's loop, so
  // that frame N always means "N frames into the device's cycle" regardless of
  // where the session's own start/end loop region sits.
  const int session_offset = std::max(0, session_frame);

  for (auto *device : device_ptrs) {
    if (!device->is_sequence()) continue;
    const int sequence_frame_count = static_cast<int>(device->frame_count());
    if (sequence_frame_count <= 0) continue;

    bool skip = false;
    std::visit(
        [&](auto &device_config) {
          if constexpr (requires {
                          device_config.sequence.value().end_frame;
                          device_config.sequence.value().start_frame;
                          device_config.sequence.value().looping;
                        }) {
            auto &seq = device_config.sequence.value();
            // session owns the clock; stop the device self-advancing
            seq.playing.set(false);

            // device's own in/out, clamped to what actually exists
            const int dev_out_cfg = seq.end_frame.value();
            const int dev_out =
                dev_out_cfg > -1
                    ? std::min(dev_out_cfg, sequence_frame_count - 1)
                    : sequence_frame_count - 1;
            const int dev_in = std::clamp(seq.start_frame.value(), 0, dev_out);
            const int dev_len = dev_out - dev_in + 1; // >= 1

            int dev_frame;
            if (seq.looping.value()) {
              // wrap within the device's own length, phase-locked to session
              dev_frame = dev_in + (session_offset % dev_len);
            } else {
              // play once then hold on the out point
              dev_frame = std::min(dev_in + session_offset, dev_out);
            }
            seq.current_frame.set(dev_frame);
          }
        },
        device->config());
    if (skip) continue;
    device->on_config_field_changed("sequence/current_frame");
  }
}

int Session::resolve_length(int total_device_frames) const {
  // total_device_frames is the max device frame_count() (cached).
  if (_playback.explicit_length > 0) return _playback.explicit_length;
  return std::max(1, total_device_frames);
}

// ---------------- master clock advance (runs on update_loop thread) ----------

void Session::advance_playback(double dt_seconds) {
  const int total_devices =
      _cached_total_frames.load(std::memory_order_acquire);

  int frame_to_apply = -1;
  bool changed = false;
  {
    std::lock_guard lk(_playback_mutex);
    if (!_playback.playing) return;

    const int length = resolve_length(total_devices);
    const int last = std::max(0, length - 1);

    const int start = std::clamp(_playback.start_frame, 0, last);
    const int end = _playback.end_frame < 0
                        ? last
                        : std::clamp(_playback.end_frame, start, last);

    // Snap into the loop region if current frame drifted outside [start, end]
    // (e.g. start/end changed while playing, or frame was preserved from before
    // a config update that narrowed the range).
    if (_playback.current_frame < start || _playback.current_frame > end) {
      _playback.current_frame = start;
      _playback_accum = 0.0;
    }

    _playback_accum += dt_seconds * std::max(1, _playback.fps);
    const int advance = static_cast<int>(_playback_accum);
    if (advance <= 0) return;
    _playback_accum -= advance;

    int next = _playback.current_frame + advance;
    if (next > end) {
      if (_playback.looping) {
        const int span = end - start + 1;
        next = span > 0 ? start + ((next - start) % span) : start;
      } else {
        next = end;
        _playback.playing = false;
      }
    }
    _playback.current_frame = next;
    frame_to_apply = next;
    changed = true;
  }

  if (frame_to_apply >= 0) scrub_devices(frame_to_apply);
  if (changed) notify_playback_changed();
}

// ---------------- update loop ----------------

// TODO check all the locks in this!!

void Session::update_loop(std::stop_token stop_token) {
  using namespace std::chrono;

  auto next_tick = steady_clock::now();
  auto last = steady_clock::now();

  std::vector<std::shared_ptr<PointCloud>> last_clouds;
  int last_total_frames = -1;
  bool last_has_sequence = false;

  while (!stop_token.stop_requested()) {
    int update_hz = 90;

    std::vector<devices::DevicePlugin *> device_ptrs;
    int total_frames = 1;
    bool has_sequence = false;
    {
      std::lock_guard lock(_workspace->config_access);
      auto &session_configs = _workspace->config.sessions;
      auto session_config =
          std::ranges::find(session_configs, id, &SessionConfiguration::id);
      // session removed: exit loop
      if (session_config == session_configs.end()) break;
      update_hz = session_config->operator_pipeline.value().update_hz.value();

      for (auto &device : _workspace->devices) {
        if (!device || device->is_discovery_instance()) continue;
        if (!includes_device(_workspace->config, *session_config, *device))
          continue;
        device_ptrs.push_back(device.get());
        if (device->is_sequence()) {
          has_sequence = true;
          total_frames =
              std::max(total_frames, static_cast<int>(device->frame_count()));
        }
      }
    }

    // publish sequence info for the Qt getters, and notify the timeline UI if
    // it changed (a sequence loaded, a device was added/removed, etc.).
    int session_total;
    {
      std::lock_guard lk(_playback_mutex);
      session_total = resolve_length(total_frames);
    }

    if (session_total != last_total_frames ||
        has_sequence != last_has_sequence) {
      last_total_frames = session_total;
      last_has_sequence = has_sequence;
      _cached_total_frames.store(
          total_frames,
          std::memory_order_release); // raw device max, for resolve_length
      _cached_session_length.store(session_total, std::memory_order_release);
      _cached_has_sequence.store(has_sequence, std::memory_order_release);
      notify_playback_changed();
    }

    // aggregate device clouds into the session pipeline only when something
    // actually changed (or a config edit forced a reprocess).
    std::vector<std::shared_ptr<PointCloud>> clouds;
    clouds.reserve(device_ptrs.size());
    for (auto *device : device_ptrs) clouds.push_back(device->point_cloud());
    bool changed = clouds.size() != last_clouds.size();
    if (!changed) {
      for (size_t i = 0; i < clouds.size(); ++i) {
        if (clouds[i] != last_clouds[i]) {
          changed = true;
          break;
        }
      }
    }
    const bool force =
        _force_reprocess.exchange(false, std::memory_order_acq_rel);

    if (changed || force) {
      last_clouds = clouds;
      auto aggregated = std::make_shared<PointCloud>();
      for (auto &cloud : clouds) {
        if (!cloud || cloud->empty()) continue;
        *aggregated += *cloud;
      }
      if (!aggregated->empty()) {
        feed_operator_pipeline(std::move(aggregated));
      } else if (_current_point_cloud.load(std::memory_order_acquire)) {
        // nothing feeds this session, so clear its cloud
        _current_point_cloud.store(nullptr, std::memory_order_release);
        _latest_render_data.store(nullptr, std::memory_order_release);
        notify_point_cloud_updated();
      }
    }

    // advance the playback master clock (scrubs all sequence devices).
    auto now = steady_clock::now();
    const double dt = duration<double>(now - last).count();
    last = now;
    advance_playback(dt);

    const auto period = duration_cast<steady_clock::duration>(
        duration<double>(1.0 / std::max(update_hz, 1)));
    next_tick += period;
    next_tick = std::max(next_tick, steady_clock::now());
    std::this_thread::sleep_until(next_tick);
  }
}

} // namespace pc