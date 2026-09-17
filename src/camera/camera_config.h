#pragma once

#include <pointcaster/core_types.h>
#include <string>

namespace pc {

struct CameraConfiguration {
  std::string id; // @hidden

  bool locked = false;       // @hidden
  bool orthographic = false; // @hidden
  bool show_grid = true;     // @hidden

  // TODO these units as floats are wrong?
  pc::float3 position;
  pc::quaternion rotation = pc::quaternion{0.989016f, -0.147809f};
  float distance = 250;
};

inline float4x4 extrinsic_from_camera_config(const CameraConfiguration &cam,
                                             const float uniform_scale = 1.0f) {
  // quaternion to rotation matrix (the orbit rotation)
  const auto &q = cam.rotation;
  float qw = q.scalar, qx = q.x, qy = q.y, qz = q.z;
  float r00 = 1 - 2 * (qy * qy + qz * qz), r01 = 2 * (qx * qy - qz * qw),
        r02 = 2 * (qx * qz + qy * qw);
  float r10 = 2 * (qx * qy + qz * qw), r11 = 1 - 2 * (qx * qx + qz * qz),
        r12 = 2 * (qy * qz - qx * qw);
  float r20 = 2 * (qx * qz - qy * qw), r21 = 2 * (qy * qz + qx * qw),
        r22 = 1 - 2 * (qx * qx + qy * qy);

  // camera world position
  float d = cam.distance;
  const auto &pos = cam.position;
  float cam_wx = pos.x + r02 * d;
  float cam_wy = pos.y + r12 * d;
  float cam_wz = pos.z + r22 * d;

  // world-to-camera
  float4x4 ext{};
  ext.values[0] = r00;
  ext.values[1] = r10;
  ext.values[2] = r20;
  ext.values[4] = r01;
  ext.values[5] = r11;
  ext.values[6] = r21;
  ext.values[8] = r02;
  ext.values[9] = r12;
  ext.values[10] = r22;

  // -R^T * cam_world_pos
  ext.values[3] = -(r00 * cam_wx + r10 * cam_wy + r20 * cam_wz) * uniform_scale;
  ext.values[7] = -(r01 * cam_wx + r11 * cam_wy + r21 * cam_wz) * uniform_scale;
  ext.values[11] =
      -(r02 * cam_wx + r12 * cam_wy + r22 * cam_wz) * uniform_scale;

  ext.values[15] = 1.0f;
  return ext;
}

} // namespace pc