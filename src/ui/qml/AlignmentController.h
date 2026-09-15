#pragma once

#include <pointcaster/core_types.h>

#include <QFutureWatcher>
#include <QMatrix4x4>
#include <QObject>
#include <QQuaternion>
#include <QVariantList>
#include <QVector3D>
#include <QtQmlIntegration/qqmlintegration.h>
#include <registration/registration.h>

namespace pc::ui::qml {

class AlignmentController : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(AlignmentController)

  Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
  Q_PROPERTY(QMatrix4x4 transform READ transform NOTIFY resultChanged)
  Q_PROPERTY(QVector3D resultPosition READ resultPosition NOTIFY resultChanged)
  Q_PROPERTY(
      QQuaternion resultRotation READ resultRotation NOTIFY resultChanged)

  Q_PROPERTY(QString transformString READ transformString NOTIFY resultChanged)

  // refinement parameters
  Q_PROPERTY(
      float maxCorrespondenceDistance READ maxCorrespondenceDistance WRITE
          setMaxCorrespondenceDistance NOTIFY refinementParamsChanged)
  Q_PROPERTY(int maxIterations READ maxIterations WRITE setMaxIterations NOTIFY
                 refinementParamsChanged)
  Q_PROPERTY(float voxelLeafSize READ voxelLeafSize WRITE setVoxelLeafSize
                 NOTIFY refinementParamsChanged)

  Q_PROPERTY(bool refining READ refining NOTIFY refiningChanged)
  Q_PROPERTY(float fitnessScore READ fitnessScore NOTIFY resultChanged)

  Q_PROPERTY(QByteArray primaryRenderData READ primaryRenderData NOTIFY
                 snapshotChanged)
  Q_PROPERTY(int primaryRenderStride READ primaryRenderStride NOTIFY
                 snapshotChanged)
  Q_PROPERTY(
      QVector3D primaryBoundsMin READ primaryBoundsMin NOTIFY snapshotChanged)
  Q_PROPERTY(
      QVector3D primaryBoundsMax READ primaryBoundsMax NOTIFY snapshotChanged)
  Q_PROPERTY(QByteArray secondaryRenderData READ secondaryRenderData NOTIFY
                 snapshotChanged)
  Q_PROPERTY(int secondaryRenderStride READ secondaryRenderStride NOTIFY
                 snapshotChanged)
  Q_PROPERTY(QVector3D secondaryBoundsMin READ secondaryBoundsMin NOTIFY
                 snapshotChanged)
  Q_PROPERTY(QVector3D secondaryBoundsMax READ secondaryBoundsMax NOTIFY
                 snapshotChanged)

public:
  explicit AlignmentController(QObject *parent = nullptr);

  QByteArray primaryRenderData() const { return _primaryRenderData; }
  int primaryRenderStride() const { return _primaryRenderStride; }
  QVector3D primaryBoundsMin() const { return _primaryBoundsMin; }
  QVector3D primaryBoundsMax() const { return _primaryBoundsMax; }
  QByteArray secondaryRenderData() const { return _secondaryRenderData; }
  int secondaryRenderStride() const { return _secondaryRenderStride; }
  QVector3D secondaryBoundsMin() const { return _secondaryBoundsMin; }
  QVector3D secondaryBoundsMax() const { return _secondaryBoundsMax; }

  bool hasResult() const { return _hasResult; }
  QMatrix4x4 transform() const { return _transform; }
  QVector3D resultPosition() const { return _position; }
  QQuaternion resultRotation() const { return _rotation; }

  QString transformString() const;

  float maxCorrespondenceDistance() const {
    return _refinementParams.max_correspondence_distance;
  }
  void setMaxCorrespondenceDistance(float v) {
    _refinementParams.max_correspondence_distance = v;
    emit refinementParamsChanged();
  }

  int maxIterations() const { return _refinementParams.max_iterations; }
  void setMaxIterations(int v) {
    _refinementParams.max_iterations = v;
    emit refinementParamsChanged();
  }

  float voxelLeafSize() const { return _refinementParams.voxel_leaf_size; }
  void setVoxelLeafSize(float v) {
    _refinementParams.voxel_leaf_size = v;
    emit refinementParamsChanged();
  }

  bool refining() const { return _refining; }
  float fitnessScore() const { return _fitnessScore; }

  Q_INVOKABLE void snapshotClouds(QObject *primaryAdapter,
                                  QObject *secondaryAdapter);

  // pairs: [{primary: vector3d, secondary: vector3d}, ...]
  // Computes transform that maps secondary to primary.
  Q_INVOKABLE void computeFromPairs(const QVariantList &pairs);

  Q_INVOKABLE void refine();

  Q_INVOKABLE void reset();

signals:
  void snapshotChanged();

  void resultChanged();
  void refinementParamsChanged();
  void refiningChanged();

private:
  std::shared_ptr<pc::PointCloud> _primarySnapshot;
  std::shared_ptr<pc::PointCloud> _secondarySnapshot;

  QByteArray _primaryRenderData;
  int _primaryRenderStride = 16;
  QVector3D _primaryBoundsMin, _primaryBoundsMax;
  QByteArray _secondaryRenderData;
  int _secondaryRenderStride = 16;
  QVector3D _secondaryBoundsMin, _secondaryBoundsMax;

  bool _hasResult = false;
  pc::float4x4 _rawTransform;
  QMatrix4x4 _transform;
  QVector3D _position;
  QQuaternion _rotation;

  pc::registration::RefinementParams _refinementParams;
  bool _refining = false;
  float _fitnessScore = 0.0f;
  QFutureWatcher<pc::registration::RefinementResult> _refinementWatcher;

  void decomposeTransform(const pc::float4x4 &matrix);
};

} // namespace pc::ui::qml