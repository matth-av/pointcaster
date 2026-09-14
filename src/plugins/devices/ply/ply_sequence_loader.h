#pragma once

#include "ply_layout.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <pointcaster/point_cloud.h>
#include <shared_mutex>
#include <string>
#include <vector>

namespace pc::devices::ply {

class PlySequenceLoader
    : public std::enable_shared_from_this<PlySequenceLoader> {
public:
  struct Config {
    size_t buffer_capacity = 60;
    size_t prefetch_ahead = 30;
    PositionUnits position_units = PositionUnits::Automatic;
  };

  bool open(const std::filesystem::path &directory, const Config &config);

  std::shared_ptr<PointCloud> get_frame(size_t frame);

  size_t frame_count() const { return _file_paths.size(); }

  void set_loop(size_t start, size_t end);

  void set_capacity(size_t buffer_capacity, size_t prefetch_ahead);

  void set_position_units(PositionUnits units);

  // file info for the first frame of a sequence
  const FileInfo &file_info() const { return _file_info; }

  void invalidate();

private:
  static constexpr size_t npos = std::numeric_limits<size_t>::max();

  std::vector<std::string> _file_paths;
  FileInfo _file_info;

  std::vector<std::shared_ptr<PointCloud>> _ring;
  std::vector<size_t> _ring_index;
  // the frame each slot has a decode queued for
  std::vector<size_t> _claimed_index;
  mutable std::shared_mutex _mutex;

  std::atomic<size_t> _generation{0};
  std::atomic<size_t> _loop_start{0};
  std::atomic<size_t> _loop_end{std::numeric_limits<size_t>::max()};
  std::atomic<PositionUnits> _position_units{PositionUnits::Automatic};
  std::atomic<size_t> _prefetch_ahead{0};

  std::shared_ptr<PointCloud> load_into_slot(size_t frame, size_t generation);
  void prefetch_from(size_t current);
};

} // namespace pc::devices::ply