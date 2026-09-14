#include "ply_sequence_loader.h"

#include <algorithm>
#include <core/logger/logger.h>
#include <pointcaster/task_pool.h>

namespace pc::devices::ply {

bool PlySequenceLoader::open(const std::filesystem::path &directory,
                             const Config &config) {
  _position_units.store(config.position_units, std::memory_order_relaxed);
  _prefetch_ahead.store(config.prefetch_ahead, std::memory_order_relaxed);
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

  _ring.assign(config.buffer_capacity, nullptr);
  _ring_index.assign(config.buffer_capacity, npos);
  _claimed_index.assign(config.buffer_capacity, npos);

  return true;
}

std::shared_ptr<PointCloud> PlySequenceLoader::get_frame(size_t frame) {
  if (frame >= _file_paths.size()) return nullptr;

  prefetch_from(frame);

  {
    std::shared_lock lock(_mutex);
    const auto slot = frame % _ring.size();
    if (_ring_index[slot] == frame && _ring[slot]) return _ring[slot];
  }

  return load_into_slot(frame, _generation.load(std::memory_order_relaxed));
}

void PlySequenceLoader::invalidate() {
  std::unique_lock lock(_mutex);
  std::ranges::fill(_ring_index, npos);
  std::ranges::fill(_claimed_index, npos);
  std::ranges::fill(_ring, nullptr);
  _generation.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<PointCloud>
PlySequenceLoader::load_into_slot(size_t frame, size_t generation) {
  auto cloud = read_point_cloud(
      _file_paths[frame], _position_units.load(std::memory_order_relaxed));

  std::unique_lock lock(_mutex);
  const auto slot = frame % _ring.size();
  if (_claimed_index[slot] == frame) _claimed_index[slot] = npos;
  if (!cloud) return nullptr;
  if (_generation.load(std::memory_order_relaxed) != generation) {
    return cloud;
  }
  _ring[slot] = cloud;
  _ring_index[slot] = frame;
  return cloud;
}

void PlySequenceLoader::set_capacity(size_t buffer_capacity,
                                     size_t prefetch_ahead) {
  std::unique_lock lock(_mutex);
  _prefetch_ahead.store(prefetch_ahead, std::memory_order_relaxed);
  if (buffer_capacity == _ring.size()) return;
  _ring.assign(buffer_capacity, nullptr);
  _ring_index.assign(buffer_capacity, npos);
  _claimed_index.assign(buffer_capacity, npos);
  _generation.fetch_add(1, std::memory_order_relaxed);
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
  const auto generation = _generation.load(std::memory_order_relaxed);

  const auto loop_start = _loop_start.load(std::memory_order_relaxed);
  const auto loop_end = std::min(_loop_end.load(std::memory_order_relaxed),
                                 _file_paths.size() - 1);
  const auto loop_range = loop_end - loop_start + 1;

  size_t capacity = 0;
  {
    std::shared_lock lock(_mutex);
    capacity = _ring.size();
  }
  if (capacity == 0) return;

  // a lookahead longer than the ring or the loop only revisits frames the
  // window already covers
  const auto effective_ahead =
      std::min({_prefetch_ahead.load(std::memory_order_relaxed), capacity - 1,
                loop_range - 1});

  for (size_t off = 1; off <= effective_ahead; ++off) {
    const auto frame_index =
        loop_start + (current - loop_start + off) % loop_range;

    {
      std::unique_lock lock(_mutex);
      const auto slot = frame_index % _ring.size();
      if (_ring_index[slot] == frame_index && _ring[slot]) continue;
      if (_claimed_index[slot] != npos) continue;
      _claimed_index[slot] = frame_index;
    }

    // this task can outlive the loader, so it holds a weak reference: lock()
    // returns null if the device has already discarded it, otherwise keeps
    // it alive until the decode finishes
    pc::task_pool().detach_task(
        [weak_self = weak_from_this(), frame_index, generation] {
          const auto self = weak_self.lock();
          if (!self) return;
          if (self->_generation.load(std::memory_order_relaxed) != generation) {
            return;
          }
          self->load_into_slot(frame_index, generation);
        });
  }
}

} // namespace pc::devices::ply