#include "camera_frame.h"

#include <logger/logger.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pointcaster/point_cloud.h>

namespace pc::camera {

CameraFrameData::CameraFrameData(size_t default_buffer_width,
                                 size_t default_buffer_height)
    : width(default_buffer_width), height(default_buffer_height) {

  const auto pixel_count = width * height;

  static constexpr color clear_color{0, 0, 0, 255};
  static constexpr float clear_depth = std::numeric_limits<float>::infinity();
  static constexpr int clear_index = -1;

  color_buffers.emplace(
      default_color_key,
      std::vector(static_cast<int>(pixel_count), clear_color));

  float_buffers.emplace(
      default_depth_key,
      std::vector(static_cast<int>(pixel_count), clear_depth));

  int_buffers.emplace(default_index_key,
                      std::vector(static_cast<int>(pixel_count), clear_index));

  pixel_hits.resize(pixel_count);
}

static_assert(sizeof(color) == 4,
              "color must be 4 bytes for zero-copy cv::Mat wrap");

void CameraFrameData::flood_fill(int fill_passes,
                                 const std::string &color_buffer_key,
                                 const std::string &depth_buffer_key,
                                 const std::string &index_buffer_key) {
  if (fill_passes <= 0) return;

  auto &colors = color_buffers.at(color_buffer_key);
  auto &depth = float_buffers.at(depth_buffer_key);
  const auto &indices = int_buffers.at(index_buffer_key);

  const int pixel_count = width * height;

  if (static_cast<int>(colors.size()) != pixel_count ||
      static_cast<int>(indices.size()) != pixel_count ||
      static_cast<int>(depth.size()) != pixel_count) {
    pc::logger()->error(
        "camera frame data is heterogenous at flood fill, aborting filter");
    return;
  }

  cv::Mat img(height, width, CV_8UC4, colors.data());
  cv::Mat depth_img(height, width, CV_32FC1, depth.data());

  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_CROSS, {3, 3});

  // Mask of pixels still empty (no projected point) — gets eroded as we fill.
  cv::Mat empty_mask(height, width, CV_8UC1);
  auto *empty_mask_ptr = empty_mask.ptr<uint8_t>();
  for (int i = 0; i < pixel_count; i++) {
    empty_mask_ptr[i] = (indices[i] < 0) ? 255 : 0;
  }

  for (int i = 0; i < fill_passes; i++) {

    // spread foreground colours into adjacent empty pixels... closes small gaps
    // in the foreground
    cv::Mat dilated;
    cv::dilate(img, dilated, kernel);
    dilated.copyTo(img, empty_mask);

    // propagate the closest neighbour's depth into empty pixels...
    // erode picks the smallest value in the neighbourhood (the closest depth to
    // the camera)
    cv::Mat closest_depth;
    cv::erode(depth_img, closest_depth, kernel);
    closest_depth.copyTo(depth_img, empty_mask);

    cv::erode(empty_mask, empty_mask, kernel);
  }
}

std::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB>>
CameraFrameData::to_pcl_organized(float fx, float fy, float cx,
                                  float cy) const {
  const auto &depth = float_buffers.at("depth");
  const auto &colors = color_buffers.at("color");
  const auto &indices = int_buffers.at("index");

  auto cloud = std::make_shared<pcl::PointCloud<pcl::PointXYZRGB>>(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  cloud->is_dense = false;

  for (int v = 0; v < height; ++v) {
    for (int u = 0; u < width; ++u) {
      int i = v * width + u;
      auto &pt = cloud->points[i];

      float z = depth[i];
      if (!std::isfinite(z)) {
        pt.x = pt.y = pt.z = std::numeric_limits<float>::quiet_NaN();
      } else {
        pt.z = z;
        pt.x = (static_cast<float>(u) - cx) * z / fx;
        pt.y = (static_cast<float>(v) - cy) * z / fy;
      }

      const auto &c = colors[i];
      pt.r = c.r;
      pt.g = c.g;
      pt.b = c.b;
    }
  }
  return cloud;
}

std::vector<uint8_t> compute_fringe_mask(int width, int height,
                                         std::span<const uint32_t> labels,
                                         const FringeMaskOptions &options) {
  const int n = width * height;
  std::vector<uint8_t> mask(n, 0);
  if (static_cast<int>(labels.size()) != n) {
    pc::logger()->error(
        "compute_fringe_mask: labels size {} != width*height {}", labels.size(),
        n);
    return mask;
  }

  const uint32_t seed_bits = options.seed_label_bits;
  for (int i = 0; i < n; ++i) {
    if (labels[i] & seed_bits) mask[i] = 255;
  }

  cv::Mat mask_mat(height, width, CV_8UC1, mask.data());
  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_CROSS, {3, 3});

  if (options.gate_label_bits != 0) {
    std::vector<uint8_t> gate(n, 0);
    const uint32_t gate_bits = options.gate_label_bits;
    for (int i = 0; i < n; ++i) {
      if (labels[i] & gate_bits) gate[i] = 255;
    }
    cv::Mat gate_mat(height, width, CV_8UC1, gate.data());
    if (options.gate_proximity_px > 0) {
      cv::Mat dilated;
      cv::dilate(gate_mat, dilated, kernel, cv::Point(-1, -1),
                 options.gate_proximity_px);
      dilated.copyTo(gate_mat);
    }
    for (int i = 0; i < n; ++i) {
      if (!gate[i]) mask[i] = 0;
    }
  }

  if (options.erosion_px <= 0) return mask;

  cv::Mat dilated;
  cv::dilate(mask_mat, dilated, kernel, cv::Point(-1, -1), options.erosion_px);
  dilated.copyTo(mask_mat);

  return mask;
}

void filter_cloud_by_pixel_mask(const CameraFrameData &frame,
                                std::span<const uint8_t> drop_mask,
                                pc::PointCloud &cloud,
                                float removal_depth_range) {
  const int n = frame.width * frame.height;
  if (static_cast<int>(drop_mask.size()) != n) {
    pc::logger()->error(
        "filter_cloud_by_pixel_mask: mask size {} != width*height {}",
        drop_mask.size(), n);
    return;
  }

  const std::size_t cloud_size = cloud.size();
  if (cloud_size == 0) return;

  std::vector<uint8_t> keep(cloud_size, 1);

  if (!frame.pixel_hits.empty()) {
    const bool depth_limited = removal_depth_range > 0.0f;
    const auto &depth_buffer = frame.float_buffers.at("depth");

    for (int i = 0; i < n; ++i) {
      if (!drop_mask[i]) continue;

      const float front_z = depth_buffer[i];

      for (const auto &hit : frame.pixel_hits[i]) {
        if (hit.point_index < 0 ||
            static_cast<std::size_t>(hit.point_index) >= cloud_size)
          continue;

        if (depth_limited && std::isfinite(front_z)) {
          if (hit.depth - front_z > removal_depth_range) continue;
        }

        keep[hit.point_index] = 0;
      }
    }
  } else {
    const auto indices_it = frame.int_buffers.find("index");
    if (indices_it == frame.int_buffers.end()) {
      pc::logger()->error(
          "filter_cloud_by_pixel_mask: frame has no index buffers");
      return;
    }
    const auto &indices = indices_it->second;
    if (static_cast<int>(indices.size()) != n) {
      pc::logger()->error(
          "filter_cloud_by_pixel_mask: index buffer size {} != width*height {}",
          indices.size(), n);
      return;
    }

    for (int i = 0; i < n; ++i) {
      if (!drop_mask[i]) continue;
      const int32_t idx = indices[i];
      if (idx >= 0 && static_cast<std::size_t>(idx) < cloud_size) {
        keep[idx] = 0;
      }
    }
  }

  std::vector<uint32_t> kept_indices;
  kept_indices.reserve(cloud_size);
  for (std::size_t read = 0; read < cloud_size; ++read) {
    if (keep[read]) kept_indices.push_back(static_cast<uint32_t>(read));
  }

  for (std::size_t i = 0; i < kept_indices.size(); i++) {
    const auto initial_index = kept_indices[i];
    if (i == initial_index) continue;
    cloud.positions[i] = cloud.positions[initial_index];
    cloud.colors[i] = cloud.colors[initial_index];
  }

  cloud.gather_attributes_into(cloud, kept_indices);
  cloud.resize(kept_indices.size());
}

} // namespace pc::camera