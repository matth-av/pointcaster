#include "ply_sequence_loader.h"

#include <algorithm>
#include <core/logger/logger.h>
#include <pointcaster/task_pool.h>

namespace pc::devices::ply {

bool PlySequenceLoader::open(const std::filesystem::path &directory,
                             const Config &config) {
  _config = config;
  _position_units.store(config.position_units, std::memory_order_relaxed);
  _file_paths.clear();
  _file_info = {};

  if (!std::filesystem::is_directory(directory)) {
    pc::logger()->error("not a directory: {}", directory.string());
    return false;
  }

  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".ply")
      _file_paths.emplace_back(entry.path().string());
  }
  std::sort(_file_paths.begin(), _file_paths.end());

  if (_file_paths.empty()) {
    pc::logger()->warn("no .ply files in {}", directory.string());
    return false;
  }

  auto file_info = scan_file_info(_file_paths.front());
  if (!file_info) return false;
  _file_info = std::move(*file_info);

  pc::logger()->info("sequence: {} frames in {}", _file_paths.size(),
                     directory.string());

  for (const auto &attribute : _file_info.attributes) {
    pc::logger()->info("ply attribute '{}' ({})", attribute.name,
                       attribute.type_name);
  }

  _ring.assign(_config.buffer_capacity, nullptr);
  _ring_index.assign(_config.buffer_capacity, npos);

  return true;
}

std::shared_ptr<PointCloud> PlySequenceLoader::get_frame(size_t frame) {
  if (frame >= _file_paths.size()) return nullptr;

  prefetch_from(frame);

  const auto slot = frame % _config.buffer_capacity;

  {
    std::shared_lock lock(_mutex);
    if (_ring_index[slot] == frame && _ring[slot]) return _ring[slot];
  }

  return load_into_slot(frame, slot);
}

void PlySequenceLoader::invalidate() {
  std::unique_lock lock(_mutex);
  std::ranges::fill(_ring_index, npos);
  std::ranges::fill(_ring, nullptr);
  _generation.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<PointCloud> PlySequenceLoader::load_into_slot(size_t frame,
                                                              size_t slot) {
  auto cloud = read_point_cloud(
      _file_paths[frame], _position_units.load(std::memory_order_relaxed));
  if (!cloud) return nullptr;
  std::unique_lock lock(_mutex);
  _ring[slot] = cloud;
  _ring_index[slot] = frame;
  return cloud;
}

void PlySequenceLoader::set_position_units(PositionUnits units) {
  const auto previous =
      _position_units.exchange(units, std::memory_order_relaxed);
  if (previous == units) return;
  invalidate();
}

void PlySequenceLoader::set_loop(size_t start, size_t end) {
  _loop_start.store(start, std::memory_order_relaxed);
  _loop_end.store(end, std::memory_order_relaxed);
}

void PlySequenceLoader::prefetch_from(size_t current) {
  const auto gen = _generation.load(std::memory_order_relaxed);

  const auto loop_start = _loop_start.load(std::memory_order_relaxed);
  const auto loop_end = std::min(_loop_end.load(std::memory_order_relaxed),
                                 _file_paths.size() - 1);
  const auto loop_range = loop_end - loop_start + 1;

  // Clamping prefetch_ahead to buffer_capacity - 1 ensures each prefetched
  // frame maps to a unique slot. Without this, frames at offsets N,
  // N+buffer_capacity, N+2*buffer_capacity,... all alias the same slot and
  // each dispatches a real mmap+copy task that loses the write race.
  const auto effective_ahead =
      std::min(_config.prefetch_ahead, _config.buffer_capacity - 1);

  for (size_t off = 1; off <= effective_ahead; ++off) {
    const auto frame_index =
        loop_start + (current - loop_start + off) % loop_range;
    const auto slot = frame_index % _config.buffer_capacity;

    {
      std::shared_lock lock(_mutex);
      if (_ring_index[slot] == frame_index && _ring[slot]) continue;
    }

    pc::task_pool().detach_task([this, frame_index, slot, gen] {
      if (_generation.load(std::memory_order_relaxed) != gen) return;
      load_into_slot(frame_index, slot);
    });
  }
}

} // namespace pc::devices::ply