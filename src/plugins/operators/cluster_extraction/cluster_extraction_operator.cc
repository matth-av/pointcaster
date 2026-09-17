#include "cluster_extraction_operator.h"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numeric>
#include <pcl/common/common.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <plugins/backend/backend_filters.h>
#include <profiling/profiling_zone.h>
#include <ranges>
#include <util/geometry_utils.h>
#include <util/string_map.h>
#include <vector>

namespace pc::operators {

void ClusterExtractionOperator::init(
    OperatorHost *host, Corrade::PluginManager::Manager<backend::BackendPlugin>
                            &backend_plugin_manager) {
  OperatorPlugin::init(host, backend_plugin_manager);
  pc::logger()->trace("Initialised ClusterExtractionOperator");
}

PipelineFramePtr ClusterExtractionOperator::process(PipelineFramePtr input) {
  using profiling::ProfilingZone;

  auto variant = load_config();
  if (!variant) return input;
  const auto &config = std::get<ClusterExtractionConfiguration>(*variant);

  ProfilingZone operator_zone("ClusterExtractionOperator");
  operator_zone.text(config.id);

  auto cloud = std::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  if (input->cloud && !input->cloud->empty()) {
    ProfilingZone conversion_zone("Convert positions to PCL");
    static constexpr auto to_pcl_point = [](const position &p) {
      return pcl::PointXYZ(static_cast<float>(p.x), static_cast<float>(p.y),
                           static_cast<float>(p.z));
    };
    const auto &positions = input->cloud->positions;
    cloud->points.resize(positions.size());
    std::ranges::transform(positions, cloud->points.begin(), to_pcl_point);
    cloud->width = static_cast<uint32_t>(cloud->points.size());
    cloud->height = 1;
    cloud->is_dense = true;
  }

  // reduce the cloud to one point per occupied voxel

  const auto leaf_size =
      static_cast<float>(std::max<int16_t>(1, config.voxel_leaf_size.mm));

  auto voxelised_cloud = std::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  if (!cloud->empty()) {
    ProfilingZone voxel_zone("Voxel grid filter");
    pcl::VoxelGrid<pcl::PointXYZ> voxel_grid;
    voxel_grid.setInputCloud(cloud);
    voxel_grid.setLeafSize(leaf_size, leaf_size, leaf_size);
    voxel_grid.setMinimumPointsNumberPerVoxel(static_cast<unsigned int>(
        std::max(0, config.minimum_points_per_voxel)));
    voxel_grid.filter(*voxelised_cloud);
  }

  if (config.filter_outlier_voxels && !voxelised_cloud->empty()) {
    ProfilingZone outlier_zone("Outlier filter");
    pcl::StatisticalOutlierRemoval<pcl::PointXYZ> outlier_filter;
    outlier_filter.setInputCloud(voxelised_cloud);
    outlier_filter.setMeanK(std::max(1, config.outlier_filter_voxel_count));
    outlier_filter.setStddevMulThresh(
        std::max(0.0f, config.outlier_filter_deviation_threshold));
    auto filtered = std::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
    outlier_filter.filter(*filtered);
    voxelised_cloud = std::move(filtered);
  }

  std::vector<pcl::PointIndices> cluster_indices;
  if (!voxelised_cloud->empty()) {
    ProfilingZone clustering_zone("Clustering");

    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree;
    {
      ProfilingZone kdtree_zone("Create KDTree");
      tree = std::make_shared<pcl::search::KdTree<pcl::PointXYZ>>();
      tree->setInputCloud(voxelised_cloud);
    }

    ProfilingZone extract_zone("Extract clusters");
    pcl::EuclideanClusterExtraction<pcl::PointXYZ> extraction;
    extraction.setClusterTolerance(
        static_cast<double>(config.cluster_tolerance.mm));
    extraction.setMinClusterSize(static_cast<pcl::uindex_t>(
        std::max(1, config.cluster_voxel_count_min)));
    extraction.setMaxClusterSize(static_cast<pcl::uindex_t>(
        std::max(1, config.cluster_voxel_count_max)));
    extraction.setSearchMethod(tree);
    extraction.setInputCloud(voxelised_cloud);
    extraction.extract(cluster_indices);
  }

  struct Cluster {
    position min;
    position max;
    std::chrono::steady_clock::time_point last_seen_time;
  };

  static constexpr auto centre_of = [](const Cluster &cluster) {
    return float3{(cluster.min.x + cluster.max.x) / 2.0f,
                  (cluster.min.y + cluster.max.y) / 2.0f,
                  (cluster.min.z + cluster.max.z) / 2.0f};
  };

  std::vector<Cluster> cluster_bounds;
  {
    ProfilingZone bounds_zone("Cluster bounds");

    const auto bounds_of = [&](const pcl::PointIndices &indices) {
      Eigen::Vector4f min_point;
      Eigen::Vector4f max_point;
      pcl::getMinMax3D(*voxelised_cloud, indices, min_point, max_point);
      return Cluster{.min = to_position(std::floor(min_point.x()),
                                        std::floor(min_point.y()),
                                        std::floor(min_point.z())),
                     .max = to_position(std::ceil(max_point.x()),
                                        std::ceil(max_point.y()),
                                        std::ceil(max_point.z())),
                     .last_seen_time = {}};
    };

    cluster_bounds = cluster_indices | std::views::transform(bounds_of) |
                     std::ranges::to<std::vector>();
  }

  // match this frame's boxes against previous clusters...

  std::vector<Cluster> updated_clusters;
  {
    ProfilingZone match_zone("Matching clusters");

    static std::mutex tracking_mutex;
    static StringMap<std::vector<Cluster>> tracking_by_operator;

    std::scoped_lock lock(tracking_mutex);
    auto &existing_clusters = tracking_by_operator[config.id];

    static constexpr auto match_clusters =
        [](const Cluster &a, const Cluster &b, float tolerance) {
          const auto centre_a = centre_of(a);
          const auto centre_b = centre_of(b);
          const auto dx = centre_a.x - centre_b.x;
          const auto dy = centre_a.y - centre_b.y;
          const auto dz = centre_a.z - centre_b.z;
          return std::sqrt(dx * dx + dy * dy + dz * dz) <= tolerance;
        };

    const auto match_tolerance = static_cast<float>(
        std::max<int16_t>(0, config.cluster_match_tolerance.mm));
    const auto timeout =
        std::chrono::milliseconds(std::max(0, config.cluster_timeout_ms));
    const auto now = std::chrono::steady_clock::now();

    std::vector<uint8_t> matched_existing_clusters(existing_clusters.size(), 0);
    updated_clusters.reserve(cluster_bounds.size() + existing_clusters.size());

    auto existing_and_matched =
        std::views::zip(existing_clusters, matched_existing_clusters);

    for (auto &new_bound : cluster_bounds) {
      for (auto [existing_cluster, matched] : existing_and_matched) {
        if (matched) continue;
        if (match_clusters(existing_cluster, new_bound, match_tolerance)) {
          // found a match to an existing cluster, which takes the new bound
          matched = 1;
          break;
        }
      }
      new_bound.last_seen_time = now;
      updated_clusters.push_back(new_bound);
    }

    // add existing clusters that were not matched but haven't expired
    for (const auto &[existing_cluster, matched] : existing_and_matched) {
      if (matched) continue;
      if (now - existing_cluster.last_seen_time <= timeout) {
        updated_clusters.push_back(existing_cluster);
      }
    }

    existing_clusters = updated_clusters;
  }

  auto clusters = std::make_shared<AabbList>();
  {
    ProfilingZone list_zone("Build AABB list");
    clusters->resize(updated_clusters.size());
    auto &bounds = clusters->bounds;

    static constexpr std::array cluster_colours{
        color{255, 96, 96, 255},   // red
        color{96, 200, 255, 255},  // blue
        color{128, 232, 128, 255}, // green
        color{255, 192, 80, 255},  // amber
        color{208, 128, 255, 255}, // violet
        color{96, 232, 216, 255},  // teal
        color{255, 128, 192, 255}, // pink
        color{200, 216, 96, 255},  // lime
    };

    auto aabb_entries = std::views::zip(
        std::views::iota(std::size_t{0}), updated_clusters,
        clusters->min_positions(), clusters->max_positions(), clusters->colors);

    for (auto [index, cluster, min_position, max_position, cluster_colour] :
         aabb_entries) {
      min_position = cluster.min;
      max_position = cluster.max;
      cluster_colour = cluster_colours[index % cluster_colours.size()];
      bounds.encompass(cluster.min);
      bounds.encompass(cluster.max);
    }
  }

  auto cluster_centroids = std::make_shared<PointCloud>();
  {
    ProfilingZone centroids_zone("Build cluster centroids");

    static constexpr auto centroid_of = [](const Cluster &cluster) {
      const auto centre = centre_of(cluster);
      return to_position(centre.x, centre.y, centre.z);
    };

    cluster_centroids->resize(updated_clusters.size());
    auto &centroids = cluster_centroids->positions;

    std::ranges::transform(updated_clusters, centroids.begin(), centroid_of);

    cluster_centroids->bounds = std::transform_reduce(
        centroids.begin(), centroids.end(), position_bounds{},
        backend::filter::merge_bounds, backend::filter::as_bounds);
  }

  auto voxels = std::make_shared<VoxelisedCloud>();
  {
    ProfilingZone voxel_cloud_zone("Build voxel cloud");
    voxels->voxel_size = static_cast<size_t>(leaf_size);
    voxels->resize(voxelised_cloud->size());
    auto &bounds = voxels->bounds;

    auto voxel_entries = std::views::zip(voxelised_cloud->points,
                                         voxels->positions, voxels->colors);

    const auto to_leaf_centre = [leaf_size](float value) {
      return (std::floor(value / leaf_size) + 0.5f) * leaf_size;
    };

    for (auto [point, voxel_position, color] : voxel_entries) {
      voxel_position =
          to_position(to_leaf_centre(point.x), to_leaf_centre(point.y),
                      to_leaf_centre(point.z));
      // TODO options to fill colour with average or brightest as well as solid
      color = {255, 255, 255, 255};
      bounds.encompass(voxel_position);
    }
  }

  // send the output data downstream the pipeline for other operators
  // that need access, they can get to it accessing the "clusters" stream
  auto output_frame = input->clone();
  output_frame->set_stream("voxels", voxels);
  output_frame->set_stream("clusters", clusters);

  // and set the output value on the config, so that publishers can watch that
  // through the config registry and react to updates
  config.voxel_count.set(static_cast<int>(voxels->size()));
  config.cluster_count.set(static_cast<int>(clusters->size()));
  config.voxels.set(std::move(voxels));
  config.clusters.set(std::move(clusters));
  config.cluster_centroids.set(std::move(cluster_centroids));

  return output_frame;
}

} // namespace pc::operators

CORRADE_PLUGIN_REGISTER(ClusterExtractionOperator,
                        pc::operators::ClusterExtractionOperator,
                        "net.pointcaster.OperatorPlugin/1.0")
