#pragma once

#include "backend_types.h"

#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StringView.h>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <camera/camera_frame.h>
#include <config/color_transform_config.h>
#include <config/transform_config.h>
#include <cpplocate/cpplocate.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <pointcaster/core_types.h>
#include <pointcaster/point_cloud.h>
#include <span>
#include <tuple>

namespace pc::backend {

class BackendPlugin : public Corrade::PluginManager::AbstractPlugin {
public:
  static Corrade::Containers::StringView pluginInterface() {
    using namespace Corrade::Containers::Literals;
    return "net.pointcaster.BackendPlugin/1.0"_s;
  }

  static Corrade::Containers::Array<Corrade::Containers::String>
  pluginSearchPaths() {
    std::filesystem::path exe_dir(cpplocate::getModulePath());
#ifdef _WIN32
    // flat windows layout: plugins/ sits beside the executable
    auto plugin_dir = exe_dir / "plugins" / "backend";
#else
    // linux bin/ layout: plugins/ sits beside the executable's parent dir
    auto plugin_dir = exe_dir.parent_path() / "plugins" / "backend";
#endif

    std::vector<Corrade::Containers::String> search_paths;

    if (std::filesystem::exists(plugin_dir)) {
      for (auto &recursive_file_node :
           std::filesystem::recursive_directory_iterator(plugin_dir)) {
        if (recursive_file_node.path().extension().string() == ".conf") {
          // for each subdirectory that contains a file named .conf,
          // treat it as a plugin directory and add it to our search paths...
          search_paths.emplace_back(
              recursive_file_node.path().parent_path().string());
        }
      }
    }

    // corrade requires at least one search path entry even when no plugins
    // exist
    if (search_paths.empty()) {
      search_paths.emplace_back(plugin_dir.string());
    }

#ifdef _WIN32
    // convert C:\style\path into posix /style/path used by corrade
    for (auto &path_entry : search_paths) {
      std::string result(path_entry.data());
      result.erase(0, 2);
      std::replace(result.begin(), result.end(), '\\', '/');
      path_entry = result;
    }
#endif

    // return results in a corrade array (required instead of vector)
    static Corrade::Containers::Array<Corrade::Containers::String> results;
    arrayClear(results);
    arrayReserve(results, search_paths.size());
    for (auto &path_string : search_paths) {
      arrayAppend(results, path_string);
    }

    return {Corrade::InPlaceInit, results};
  }

  explicit BackendPlugin(Corrade::PluginManager::AbstractManager &manager,
                         Corrade::Containers::StringView plugin)
      : Corrade::PluginManager::AbstractPlugin{manager, plugin} {}

  virtual ~BackendPlugin() = default;

  // since plugins are created by a factory, we implement a custom init() which
  // must be called and can optionally be overriden if backend devices need to
  // allocate memory up front
  virtual void init([[maybe_unused]] const size_t point_count = 0) {};

  virtual void
  transform_point_cloud(const PointCloud &input_cloud, PointCloud &output_cloud,
                        const TransformConfiguration &transform,
                        const ColorTransformConfiguration &color_transform,
                        const pc::float4x4 &world_transform = {}) const = 0;

  // crop a cloud to an aabb and/or measure what falls inside it
  virtual BoundsFilterResult
  filter_to_bounds(const PointCloud &input_cloud, PointCloud &output_cloud,
                   const position_bounds &bounds,
                   BoundsFilterOptions options = {}) const = 0;

  virtual void project_transform_frame_data(
      std::span<const uint16_t> input_depth_frame,
      std::span<const color_rgb> input_rgb_frame, PointCloud &output_cloud,
      const CameraIntrinsics &color_intrinsics,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform,
      const pc::float4x4 &world_transform = {}) const = 0;

  // projects through an xy table instead of pinhole intrinsics, for
  // cameras that return their raw data with lens distortion still applied
  // (e.g. k4a)
  virtual void project_transform_frame_data(
      std::span<const uint16_t> input_depth_frame,
      std::span<const color_bgra> input_bgra_frame, PointCloud &output_cloud,
      std::shared_ptr<const XyTable> xy_table,
      const TransformConfiguration &transform,
      const ColorTransformConfiguration &color_transform,
      const pc::float4x4 &world_transform = {}) const = 0;

  virtual void pack_render_buffer(const PointCloud &cloud,
                                  std::span<std::byte> output) const = 0;

  virtual void project_frame(const PointCloud &cloud,
                             camera::CameraFrameData &output,
                             camera::FrameProjectionArgs projection) const = 0;
};

} // namespace pc::backend