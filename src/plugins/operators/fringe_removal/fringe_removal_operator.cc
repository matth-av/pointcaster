#include "fringe_removal_operator.h"
#include "fringe_removal_config.h"

#include <algorithm>
#include <camera/camera_frame.h>
#include <camera/look_at_camera.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <pcl/features/organized_edge_detection.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <profiling/profiling_zone.h>
#include <utility>
#include <variant>
#include <vector>

namespace pc::operators {

void FringeRemovalOperator::init(
    OperatorHost *host, Corrade::PluginManager::Manager<backend::BackendPlugin>
                            &backend_plugin_manager) {
  OperatorPlugin::init(host, backend_plugin_manager);
  // TODO need to update all config including position
  // need to allocate resources used in process here
  _camera.update_config(config().camera);
  pc::logger()->trace("Initialised FringeRemovalOperator");
}

void FringeRemovalOperator::on_config_field_changed(std::string_view path) {
  OperatorPlugin::on_config_field_changed(path);
  if (auto config_ptr = load_config()) {
    const auto config = std::get<FringeRemovalConfiguration>(*config_ptr);
    if (path.contains("camera")) {
      _camera.update_config(config.camera);
    }
  }
  if (path.contains("camera")) {
  }
}

PipelineFramePtr FringeRemovalOperator::process(PipelineFramePtr input) {
  using profiling::ProfilingZone;

  auto variant = load_config();
  if (!variant) return input;

  const auto &config = std::get<FringeRemovalConfiguration>(*variant);

  // TODO is this sync on every process necessary?
  _camera.update_config(config.camera);

  if (!config.remove_occluding_fringe && !config.remove_canny_fringe) {
    return input;
  }

  if (!_current_backend) {
    pc::logger()->error("No backend set for {}", std::string_view{plugin()});
    return input;
  }

  ProfilingZone operator_zone("FringeRemovalOperator");
  operator_zone.text(config.id);

  {
    ProfilingZone projection_zone("_camera.project");
    _camera.project(*input->cloud, _current_backend);
    _input_image = {.name = "Input", .frame_data = _camera.result()};
  }

  auto &input_frame = *_input_image.frame_data.value();
  const auto &input_colors = input_frame.main_color_buffer();

  const auto &[fx, fy, cx, cy, width, height, extrinsic] =
      _camera.projection_args();

  const auto pixel_count =
      static_cast<size_t>(width) * static_cast<size_t>(height);

  // given our 2d camera projected image, we can now generate an RGBD frame from
  // this camera's perspective to use in organised point-cloud algorithms
  using RGBDFramePtr = decltype(input_frame.to_pcl_organized(
      std::declval<float>(), std::declval<float>(), std::declval<float>(),
      std::declval<float>()));

  RGBDFramePtr pcl_rgbd_cloud;
  {
    ProfilingZone input_frame_to_pcl_zone("input_frame::to_pcl_organized");
    pcl_rgbd_cloud = input_frame.to_pcl_organized(fx, fy, cx, cy);
  }

  // now use PCL to detect edges
  pcl::PointCloud<pcl::Label> edge_labels;
  std::vector<pcl::PointIndices> edge_label_indices;
  {
    pcl::OrganizedEdgeFromRGB<pcl::PointXYZRGB, pcl::Label> edge_detector;

    edge_detector.setInputCloud(pcl_rgbd_cloud);
    edge_detector.setDepthDisconThreshold(config.depth_threshold);
    edge_detector.setMaxSearchNeighbors(config.max_search_neighbors);
    edge_detector.setRGBCannyLowThreshold(config.canny_low);
    edge_detector.setRGBCannyHighThreshold(config.canny_high);

    {
      ProfilingZone edge_detector_zone("pcl::edge_detector::compute");
      edge_detector.compute(edge_labels, edge_label_indices);
    }
  }

  {
    ProfilingZone edge_visualisation_zone("create_edge_visualisation");

    // TODO we could probably make the edge_visualisation_frame some kind of
    // pre-allocated storage so we dont allocate memory each frame, but its fine
    // for now

    auto edge_visualisation_frame = std::make_shared<camera::CameraFrameData>(
        static_cast<size_t>(width), static_cast<size_t>(height));

    auto &edge_visualisation_colors =
        edge_visualisation_frame->main_color_buffer();

    for (size_t i = 0; i < pixel_count; ++i) {
      uint32_t lbl = edge_labels[i].label;
      if (lbl & 4)
        edge_visualisation_colors[i] = {255, 0, 0, 255};
      else if (lbl & 2)
        edge_visualisation_colors[i] = {0, 255, 0, 255};
      else if (lbl & 1)
        edge_visualisation_colors[i] = {0, 0, 255, 255};
      else if (lbl & 16)
        edge_visualisation_colors[i] = {255, 255, 0, 255};
      else
        edge_visualisation_colors[i] = input_colors[i];
    }

    int counts[5] = {};
    for (size_t i = 0; i < pixel_count; ++i) {
      uint32_t lbl = edge_labels[i].label;
      if (lbl & 1) counts[0]++;
      if (lbl & 2) counts[1]++;
      if (lbl & 4) counts[2]++;
      if (lbl & 8) counts[3]++;
      if (lbl & 16) counts[4]++;
    }

    _edge_detector_image = {.name = "Edge Detector",
                            .frame_data = std::move(edge_visualisation_frame)};
  }

  // Build drop mask from edge labels
  std::vector<uint8_t> drop_mask(pixel_count, 0);
  {
    ProfilingZone drop_mask_zone("build_drop_mask");

    // TODO could possibly make this a member variable
    std::vector<uint32_t> label_bits_buffer(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
      label_bits_buffer[i] = edge_labels[i].label;
    }

    auto merge = [&](const std::vector<uint8_t> &m) {
      for (size_t i = 0; i < pixel_count; ++i) {
        if (m[i]) drop_mask[i] = 255;
      }
    };

    if (config.remove_occluding_fringe) {
      ProfilingZone compute_occluding_zone("compute_occluding_mask");

      camera::FringeMaskOptions opts{
          .seed_label_bits = camera::EdgeOccluding | camera::EdgeNanBoundary,
          .erosion_px = config.occluding_erosion_px,
      };
      merge(
          camera::compute_fringe_mask(width, height, label_bits_buffer, opts));
    }

    if (config.remove_canny_fringe) {
      ProfilingZone compute_canny_zone("compute_canny_mask");

      camera::FringeMaskOptions opts{
          .seed_label_bits = camera::EdgeOccluding,
          .gate_label_bits = camera::EdgeRgbCanny,
          .gate_proximity_px = config.canny_proximity_px,
          .erosion_px = config.canny_erosion_px,
      };
      merge(
          camera::compute_fringe_mask(width, height, label_bits_buffer, opts));
    }
  }

  // Smooth the combined drop mask

  {
    ProfilingZone smooth_mask_zone("smooth_mask");

    cv::Mat mask_img(height, width, CV_8UC1, drop_mask.data());

    const int close_r = config.morph_close_radius;
    if (close_r > 0) {
      auto kernel = cv::getStructuringElement(
          cv::MORPH_ELLIPSE, {close_r * 2 + 1, close_r * 2 + 1});
      cv::morphologyEx(mask_img, mask_img, cv::MORPH_CLOSE, kernel);
    }

    const int blur = config.blur_radius;
    if (blur > 0) {
      const int k = blur * 2 + 1;
      cv::GaussianBlur(mask_img, mask_img, {k, k}, 0);
      cv::threshold(mask_img, mask_img,
                    static_cast<double>(config.blur_threshold), 255,
                    cv::THRESH_BINARY);
    }
  }

  // Drop mask visualisation

  {
    ProfilingZone mask_visualisation_zone("create_mask_visualisation");

    const auto &input_colors = input_frame.main_color_buffer();

    auto drop_visualisation = std::make_shared<camera::CameraFrameData>(
        static_cast<size_t>(width), static_cast<size_t>(height));
    auto &vis_colors = drop_visualisation->main_color_buffer();

    for (size_t i = 0; i < pixel_count; ++i) {
      vis_colors[i] =
          drop_mask[i] ? pc::color{255, 0, 255, 255} : input_colors[i];
    }

    _drop_mask_image = {.name = "Removed Pixels",
                        .frame_data = std::move(drop_visualisation)};
  }

  // apply the output pixel mask to copy of input point cloud to create our
  // result
  auto output_frame = input->clone();
  auto output_cloud = std::make_shared<PointCloud>(*input->cloud);
  {
    ProfilingZone filter_pointcloud_zone("filter_cloud_by_mask");
    camera::filter_cloud_by_pixel_mask(input_frame, drop_mask, *output_cloud,
                                       config.removal_depth_range);
  }
  output_frame->cloud = std::move(output_cloud);
  return output_frame;
}

} // namespace pc::operators

CORRADE_PLUGIN_REGISTER(FringeRemovalOperator,
                        pc::operators::FringeRemovalOperator,
                        "net.pointcaster.OperatorPlugin/1.0")