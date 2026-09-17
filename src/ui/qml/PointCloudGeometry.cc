#include "PointCloudGeometry.h"
#include <QVector3D>

namespace pc::ui::qml {

PointCloudGeometry::PointCloudGeometry() : QQuick3DGeometry() {
  update();
}

void PointCloudGeometry::setPointCloudAdapter(QObject *adapter) {
  if (_pointCloudAdapter == adapter) return;
  _pointCloudAdapter = adapter;
  emit pointCloudAdapterChanged();
  updateGeometry();
}

void PointCloudGeometry::setEnabled(bool enabled) {
  if (_enabled == enabled) return;
  if (_enabled && !enabled) reset();
  _enabled = enabled;
  emit enabledChanged();
}

void PointCloudGeometry::updateGeometry() {
  auto *adapter = dynamic_cast<PointCloudAdapter *>(_pointCloudAdapter.data());
  if (!_enabled || !adapter) {
    return;
  }

  auto cloud = adapter->point_cloud();
  auto render_buffer = adapter->render_data();

  if (!cloud || cloud->empty() || !render_buffer || render_buffer->empty()) {
    reset();
    return;
  }

  _vertexBuffer =
      QByteArray(reinterpret_cast<const char *>(render_buffer->data()),
                 static_cast<qsizetype>(render_buffer->size()));

  // if the stride is wider than the base size, we know a point scale is also
  // packed in the buffer too...
  _stride = static_cast<int>(render_buffer->size() / cloud->size());
  const auto had_point_scale = _hasPointScale;
  _hasPointScale = _stride >= 20;

  clear();
  setVertexData(_vertexBuffer);
  setStride(_stride);
  setPrimitiveType(QQuick3DGeometry::PrimitiveType::Points);
  addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
  addAttribute(Attribute::TexCoord0Semantic, 12, Attribute::F32Type);
  if (had_point_scale != _hasPointScale) emit hasPointScaleChanged();

  const auto &bounds = cloud->bounds;
  _boundsMin = QVector3D(bounds.min.x, bounds.min.y, bounds.min.z);
  _boundsMax = QVector3D(bounds.max.x, bounds.max.y, bounds.max.z);
  _boundsCenter = ((_boundsMin + _boundsMax) * 0.5) * 0.1;
  setBounds(_boundsMin, _boundsMax);
  emit boundsChanged();
  update();
}

void PointCloudGeometry::reset() {
  clear();
  _boundsMin = {};
  _boundsMax = {};
  _boundsCenter = {};
  setBounds(_boundsMin, _boundsMax);
  emit boundsChanged();
  update();
}

void PointCloudGeometry::setStaticData(const QByteArray &vertexData, int stride,
                                       const QVector3D &boundsMin,
                                       const QVector3D &boundsMax) {
  _vertexBuffer = vertexData;
  _stride = stride;
  clear();
  setVertexData(_vertexBuffer);
  setStride(_stride);
  setPrimitiveType(QQuick3DGeometry::PrimitiveType::Points);
  addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
  addAttribute(Attribute::TexCoord0Semantic, 12, Attribute::F32Type);
  _boundsMin = boundsMin;
  _boundsMax = boundsMax;
  _boundsCenter = ((_boundsMin + _boundsMax) * 0.5) * 0.1;
  setBounds(_boundsMin, _boundsMax);
  emit boundsChanged();
  update();
}

QVector3D PointCloudGeometry::pointPosition(int index) const {
  const int byteOffset = index * _stride;
  if (byteOffset + _stride > _vertexBuffer.size()) return {};

  const char *data = _vertexBuffer.constData() + byteOffset;
  int32_t raw[2];
  std::memcpy(&raw[0], data, 4);
  std::memcpy(&raw[1], data + 4, 4);

  float px = static_cast<float>(static_cast<int16_t>(raw[0] & 0xFFFF));
  float py = static_cast<float>(raw[0] >> 16);
  float pz = static_cast<float>(static_cast<int16_t>(raw[1] & 0xFFFF));

  return QVector3D(px, py, pz) * 0.1f;
}

} // namespace pc::ui::qml