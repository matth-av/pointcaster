#pragma once

#include <config/core_types_reflection.h>
#include <config/output_value.h>
#include <plugins/backend/backend_types.h>
#include <pointcaster/point_cloud.h>
#include <rfl/Literal.hpp>
#include <string>

namespace pc::operators {

class ClusterExtractionOperator;

struct ClusterExtractionConfiguration {
  std::string id;     // @hidden
  std::string label;  // @hidden
  bool active = true; // @hidden

  // voxelisation
  length voxel_leaf_size = length{200}; // @minmax(50, 1000)
  int minimum_points_per_voxel = 0;     // @minmax(0, 1000)

  bool filter_outlier_voxels = false;
  int outlier_filter_voxel_count = 30;             // @minmax(1, 200)
  float outlier_filter_deviation_threshold = 1.0f; // @minmax(0, 10)

  // clustering
  length cluster_tolerance = length{270}; // @minmax(120, 1200)
  int cluster_voxel_count_min = 10;       // @minmax(3, 1000)
  int cluster_voxel_count_max = 100;      // @minmax(3, 1000)

  length cluster_match_tolerance = length{500}; // @minmax(0, 5000)
  int cluster_timeout_ms = 100;                 // @minmax(0, 5000)

  // output fields
  Output<VoxelisedCloudPtr> voxels;
  Output<int> voxel_count = 0;
  Output<AabbListPtr> clusters;
  Output<PointCloudPtr> cluster_centroids;
  Output<int> cluster_count = 0;

  BackendType backend = BackendType::CPU; // @hidden

  using OperatorType = ClusterExtractionOperator;
  using Tag = rfl::Literal<"clusterExtraction">;
  static constexpr auto PluginName = "ClusterExtractionOperator";
};

} // namespace pc::operators
