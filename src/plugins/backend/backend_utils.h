#pragma once

#include "backend_types.h"

#include <cmath>
#include <cstdint>
#include <pointcaster/core_types.h>

namespace pc::backend::util {

#ifdef __CUDACC__
__host__ __device__
#endif
    inline CameraIntrinsics make_camera_intrinsics(float fx, float fy, float cx,
                                                   float cy,
                                                   size_t frame_width) {
  CameraIntrinsics cam;
  cam.fx = fx;
  cam.fy = fy;
  cam.cx = cx;
  cam.cy = cy;

  cam.inv_fx = 1.0f / fx;
  cam.inv_fy = 1.0f / fy;

  cam.frame_width = frame_width;

  return cam;
}

#ifdef __CUDACC__
__host__ __device__
#endif
    inline auto round_pos(float v) {
#ifdef __CUDACC__
  return llrintf(v);
#else
  return std::lroundf(v);
#endif
}

#ifdef __CUDACC__
__host__ __device__
#endif
    inline position project_2d_to_3d(int px, int py, uint16_t depth,
                                     const CameraIntrinsics &cam) {
  const float z = static_cast<float>(depth);
  const float x = (static_cast<float>(px) - cam.cx) * z * cam.inv_fx;
  const float y = (static_cast<float>(py) - cam.cy) * z * cam.inv_fy;
  return {static_cast<int16_t>(round_pos(x)),
          static_cast<int16_t>(-round_pos(y)),
          static_cast<int16_t>(-round_pos(z))};
}

#ifdef __CUDACC__
__host__ __device__
#endif
    inline position project_2d_to_3d(int px, int py, uint16_t depth,
                                     const CameraIntrinsics &cam,
                                     const Extrinsics &ext) {
  const float z = static_cast<float>(depth);
  const float x = (static_cast<float>(px) - cam.cx) * z * cam.inv_fx;
  const float y = (static_cast<float>(py) - cam.cy) * z * cam.inv_fy;
  return {.x = static_cast<int16_t>(
              round_pos(ext.r[0] * x + ext.r[1] * y + ext.r[2] * z + ext.t[0])),
          .y = static_cast<int16_t>(-round_pos(ext.r[3] * x + ext.r[4] * y +
                                               ext.r[5] * z + ext.t[1])),
          .z = static_cast<int16_t>(-round_pos(ext.r[6] * x + ext.r[7] * y +
                                               ext.r[8] * z + ext.t[2]))};
}

#ifdef __CUDACC__
__host__ __device__
#endif
    inline position project_2d_to_3d(const float2 &xy_scale, uint16_t depth) {
  const float z = static_cast<float>(depth);
  return {static_cast<int16_t>(round_pos(xy_scale.x * z)),
          static_cast<int16_t>(-round_pos(xy_scale.y * z)),
          static_cast<int16_t>(-round_pos(z))};
}

#ifdef __CUDACC__
__host__ __device__
#endif
    inline bool is_valid_xy_scale(const float2 &xy_scale) {
  return xy_scale.x != invalid_xy_scale_component;
}

// a bgra pixel of all zeros is how colour-to-depth mapping marks a depth pixel
// no colour pixel landed on (a real black pixel comes out as 1, 0, 0, 0)
#ifdef __CUDACC__
__host__ __device__
#endif
    inline bool has_color(const color_bgra &pixel) {
  return pixel.b != 0 || pixel.g != 0 || pixel.r != 0 || pixel.a != 0;
}

} // namespace pc::backend::util