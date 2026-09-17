#include "ply_device.h"
#include "plugins/devices/device_variants.h"

#include <core/logger/logger.h>
// #include <core/profiling/profiling_zone.h>
#include <cstring>
#include <filesystem>
#include <plugins/backend/backend_types.h>
#include <plugins/backend/cpu/cpu_backend.h>
#include <plugins/devices/device_tree.h>
#include <pointcaster/point_cloud.h>
#include <ranges>
#include <workspace/workspace.h>

namespace pc::devices {

// using pc::profiling::ProfilingZone;

namespace {

ply::PlySequenceLoader::Config
sequence_loader_config(PlyDeviceConfiguration &config) {
  auto &sequence = config.sequence;
  return {
      .buffer_capacity =
          static_cast<size_t>(std::max(8, sequence.buffer_capacity)),
      .prefetch_ahead =
          static_cast<size_t>(std::max(1, sequence.prefetch_ahead)),
      .position_units = config.position_units,
      .attributes = config.attributes,
  };
}

void seed_attribute_rows(PlyDeviceConfiguration &config,
                         const ply::FileInfo &info) {
  auto &rows = config.attributes;
  for (const auto &attribute : info.attributes) {
    const auto existing = std::ranges::find_if(
        rows, [&](const auto &row) { return row.name == attribute.name; });
    if (existing != rows.end()) continue;
    rows.push_back({.name = attribute.name,
                    .target = attribute.name == "pscale"
                                  ? AttributeTarget::PointScale
                                  : AttributeTarget::None});
  }
}

} // namespace

PlyDevice::~PlyDevice() {
  if (_sequence_loader) _sequence_loader->invalidate();
  _tick_thread.request_stop();
  if (_tick_thread.joinable()) _tick_thread.join();
}

bool PlyDevice::load(std::string_view url) {
  std::lock_guard lock(_device_mutex);
  pc::logger()->trace("PlyDevice::load: enter url='{}' tid={}", url,
                      std::hash<std::thread::id>{}(std::this_thread::get_id()));

#ifdef _WIN32
  static constexpr std::string file_prefix = "file:///";
#else
  static constexpr std::string file_prefix = "file://";
#endif

  auto path_str = std::string(
      url.starts_with(file_prefix) ? url.substr(file_prefix.size()) : url);

  if (std::filesystem::is_directory(path_str)) {
    pc::logger()->trace("PlyDevice::load: directory mode");
    if (!load_directory(path_str)) return false;
    _loaded_file_path = std::string(url);
    pc::logger()->trace("PlyDevice::load: directory load done");
    return true;
  }

  pc::logger()->trace(
      "PlyDevice::load: single-file mode, resetting sequence loader");
  _sequence_loader.reset();

  pc::logger()->trace("PlyDevice::load: parsing '{}'", path_str);

  auto &config = std::get<PlyDeviceConfiguration>(_config);
  const auto position_units = config.position_units;

  // the header is scanned first so attribute choices the workspace already
  // holds apply on read
  if (const auto info = ply::scan_file_info(path_str)) {
    _file_info = *info;
    seed_attribute_rows(config, _file_info);
  }

  auto input_cloud =
      ply::read_point_cloud(path_str, position_units, config.attributes);
  if (!input_cloud) {
    pc::logger()->error("PlyDevice::load: could not read '{}'", path_str);
    return false;
  }

  pc::logger()->trace("PlyDevice::load: read {} points, applying transform",
                      input_cloud->size());

  _input_cloud = std::move(input_cloud);
  _loaded_file_path = std::string(url);
  apply_transform();
  pc::logger()->trace("PlyDevice::load: done");
  return true;
}

bool PlyDevice::load_directory(const std::filesystem::path &dir) {
  auto &config = std::get<PlyDeviceConfiguration>(_config);

  _input_cloud.reset();
  _sequence_loader = std::make_shared<ply::PlySequenceLoader>();

  if (!_sequence_loader->open(dir, sequence_loader_config(config))) {
    _sequence_loader.reset();
    // _status = DeviceStatus::Error;
    return false;
  }

  _current_frame = 0;
  _frame_accumulator = 0.f;
  // _status = DeviceStatus::Loaded;

  _file_info = _sequence_loader->file_info();
  seed_attribute_rows(config, _file_info);

  _input_cloud = _sequence_loader->get_frame(0);
  apply_transform();
  return true;
}

void PlyDevice::on_session_membership_changed(bool in_any_session) {
  if (!in_any_session) return;

  std::string path_to_load;
  {
    std::lock_guard lock(_device_mutex);
    if (!std::holds_alternative<PlyDeviceConfiguration>(_config)) return;
    const auto &file_config = std::get<PlyDeviceConfiguration>(_config).file;
    if (file_config.path.empty() || file_config.path == _loaded_file_path)
      return;
    path_to_load = file_config.path;
  }
  load(path_to_load);
}

void PlyDevice::reload() {
  const auto &config = std::get<PlyDeviceConfiguration>(_config);
  load(config.file.path);
}

void PlyDevice::tick(float delta_time) {
  std::lock_guard lock(_device_mutex);
  if (!_sequence_loader) return;

  auto &config = std::get<PlyDeviceConfiguration>(_config);
  auto &seq = config.sequence;

  if (!seq.playing) return;

  _frame_accumulator += delta_time * static_cast<float>(seq.frame_rate);
  const auto advance = static_cast<int>(_frame_accumulator);
  if (advance == 0) return;
  _frame_accumulator -= static_cast<float>(advance);

  const auto total = static_cast<int>(_sequence_loader->frame_count());
  const auto start = std::clamp(seq.start_frame, 0, total - 1);
  const auto end = (seq.end_frame < 0)
                       ? total - 1
                       : std::clamp(seq.end_frame, start, total - 1);
  const auto range = end - start + 1;

  // once a non-looping sequence has reached its end, stay paused there even
  // if "playing" keeps getting set to true externally (e.g. held high via OSC)
  if (!seq.looping && _current_frame >= end) {
    seq.playing = false;
    return;
  }

  if (_current_frame < start || _current_frame > end) _current_frame = start;

  auto next = _current_frame + advance;

  if (next > end) {
    if (seq.looping) {
      next = start + (next - start) % range;
    } else {
      next = end;
      seq.playing = false;
    }
  }

  if (next == _current_frame) return;
  _current_frame = next;
  seq.current_frame = _current_frame;

  _sequence_loader->set_loop(static_cast<size_t>(start),
                             static_cast<size_t>(end));

  reload_current_frame();
}

void PlyDevice::reload_current_frame() {
  if (!_sequence_loader) return;
  if (auto frame =
          _sequence_loader->get_frame(static_cast<size_t>(_current_frame))) {
    _input_cloud = std::move(frame);
    apply_transform();
  }
}

size_t PlyDevice::frame_count() const {
  return _sequence_loader ? _sequence_loader->frame_count() : 1;
}

void PlyDevice::on_config_field_changed(std::string_view path) {
  DevicePlugin::on_config_field_changed(path);

  std::unique_lock lock(_device_mutex);
  auto &config = std::get<PlyDeviceConfiguration>(_config);

  // file path changed... reload
  if (path.contains("file")) {
    auto file_config = config.file;
    if (file_config.path != _loaded_file_path) {
      lock.unlock();
      if (!load(file_config.path)) {
        lock.lock();
        // rollback
        file_config.path = _loaded_file_path;
        config.file = file_config;
      }
      return;
    }
  }

  if (path.contains("position_units")) {
    if (_sequence_loader) {
      _sequence_loader->set_position_units(config.position_units);
      reload_current_frame();
      return;
    }
    const auto file_path = config.file.path;
    lock.unlock();
    load(file_path);
    return;
  }

  if (path.contains("attributes")) {
    if (_sequence_loader) {
      _sequence_loader->set_attributes(config.attributes);
      reload_current_frame();
    } else {
      const auto file_path = config.file.path;
      lock.unlock();
      load(file_path);
    }
    return;
  }

  // sequence config changes
  if (_sequence_loader && path.contains("sequence")) {
    if (path.contains("buffer_capacity") || path.contains("prefetch_ahead")) {
      const auto loader_config = sequence_loader_config(config);
      _sequence_loader->set_capacity(loader_config.buffer_capacity,
                                     loader_config.prefetch_ahead);
      return;
    }

    // scrub...
    if (path.contains("current_frame")) {
      auto &sequence_config = config.sequence;
      const auto total = static_cast<int>(_sequence_loader->frame_count());
      const auto start = std::clamp(sequence_config.start_frame, 0, total - 1);
      const auto end =
          (sequence_config.end_frame < 0)
              ? total - 1
              : std::clamp(sequence_config.end_frame, start, total - 1);

      _current_frame = std::clamp(sequence_config.current_frame, start, end);
      if (_current_frame != sequence_config.current_frame) {
        sequence_config.current_frame = _current_frame;
      }

      _sequence_loader->set_loop(static_cast<size_t>(start),
                                 static_cast<size_t>(end));
      reload_current_frame();
      return;
    }
  }

  // transform / color / operator changes... trigger re-transform current frame
  if (path.empty() || path.contains("transform") || path.contains("color") ||
      path.contains("operator")) {
    apply_transform();
  }
}

void PlyDevice::update_config(
    const devices::DeviceConfigurationVariant &config) {
  pc::logger()->trace("PlyDevice::update_config: enter tid={}",
                      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  std::string path_to_load;
  bool need_tick_thread = false;
  {
    std::lock_guard lock(_device_mutex);
    DevicePlugin::update_config(config);
    if (!std::holds_alternative<PlyDeviceConfiguration>(_config)) {
      pc::logger()->trace(
          "PlyDevice::update_config: variant not Ply, returning");
      return;
    }
    const auto &cfg = std::get<PlyDeviceConfiguration>(_config);
    const auto &file_config = cfg.file;
    pc::logger()->trace("PlyDevice::update_config: id='{}' path='{}' "
                        "loaded='{}'",
                        cfg.id, file_config.path, _loaded_file_path);
    if (in_any_session() && !file_config.path.empty() &&
        file_config.path != _loaded_file_path) {
      path_to_load = file_config.path;
    }
    need_tick_thread = !_tick_thread.joinable();
  }

  if (!path_to_load.empty()) {
    pc::logger()->trace("PlyDevice::update_config: loading '{}'", path_to_load);
    load(path_to_load);
    pc::logger()->trace("PlyDevice::update_config: load returned");
  }

  if (need_tick_thread) {
    pc::logger()->trace("PlyDevice::update_config: spawning tick thread");
    _tick_thread = std::jthread([this](std::stop_token stop) {
      using namespace std::chrono;
      auto last = steady_clock::now();
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(milliseconds(10));
        auto now = steady_clock::now();
        float dt = duration<float>(now - last).count();
        last = now;
        if (in_any_session()) tick(dt);
      }
    });
  }
  pc::logger()->trace("PlyDevice::update_config: done");
}

std::shared_ptr<PointCloud> PlyDevice::point_cloud() {
  return _current_point_cloud.load(std::memory_order_acquire);
}

void PlyDevice::apply_transform() {
  pc::logger()->trace("PlyDevice::apply_transform: enter tid={}",
                      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  if (!_input_cloud) {
    pc::logger()->trace(
        "PlyDevice::apply_transform: no input cloud, returning");
    return;
  }

  const auto config = std::get<PlyDeviceConfiguration>(_config);
  const auto point_count = _input_cloud->size();

  auto *backend = current_backend();
  if (!backend) {
    pc::logger()->error("PlyDevice::apply_transform: uninitialised backend");
    return;
  }

  pc::logger()->trace("PlyDevice::apply_transform: id='{}' points={}, "
                      "resolving world transform",
                      config.id, point_count);

  pc::float4x4 world;
  {
    std::scoped_lock lock(_workspace->config_access);
    world =
        pc::devices::effective_world_transform(_workspace->config, config.id);
  }
  pc::logger()->trace(
      "PlyDevice::apply_transform: world resolved, transforming");

  auto transformed_cloud = std::make_shared<PointCloud>();
  transformed_cloud->resize(point_count);

  backend->transform_point_cloud(*_input_cloud, *transformed_cloud,
                                 config.transform, config.color, world);

  pc::logger()->trace("PlyDevice::apply_transform: feeding operator pipeline");
  feed_operator_pipeline(std::move(transformed_cloud));
  pc::logger()->trace("PlyDevice::apply_transform: done");
}

void PlyDevice::on_pipeline_output(operators::PipelineFramePtr output_frame) {
  if (!output_frame) return;
  auto cloud = output_frame->cloud;
  if (!cloud) return;
  // TODO maybe we need conditional rendering?
  if (auto *cpu = cpu_backend()) {
    auto buf = std::make_shared<std::vector<std::byte>>(
        cloud->size() * render_vertex_stride(*cloud));
    cpu->pack_render_buffer(*cloud, *buf);
    _latest_render_data.store(std::move(buf), std::memory_order_release);
  }
  _current_point_cloud.store(std::move(cloud), std::memory_order_release);
  notify_point_cloud_updated();
}

ply::FileInfo PlyDevice::file_info() const {
  std::lock_guard lock(_device_mutex);
  return _file_info;
}

ImportedAttributes PlyDevice::imported_attributes() const {
  std::lock_guard lock(_device_mutex);
  ImportedAttributes imported;
  imported.attributes.reserve(_file_info.attributes.size());
  for (const auto &attribute : _file_info.attributes) {
    imported.attributes.push_back({attribute.name, attribute.type_name});
  }
  return imported;
}

} // namespace pc::devices

CORRADE_PLUGIN_REGISTER(PlyDevice, pc::devices::PlyDevice,
                        "net.pointcaster.DevicePlugin/1.0")