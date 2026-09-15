#include "AlignmentController.h"
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QtConcurrent>
#include <exception>
#include <logger/logger.h>
#include <registration/registration.h>
#include <util/geometry_utils.h>
#include <vector>

namespace pc::ui::qml {

AlignmentController::AlignmentController(QObject *parent) : QObject(parent) {
  connect(&_refinementWatcher,
          &QFutureWatcher<pc::registration::RefinementResult>::finished, this,
          [this]() {
            try {
              auto result = _refinementWatcher.result();
              if (result.converged) {

                // ********
                // TODO is this necessary?
                // scale translation back to scene-graph units (*0.1)
                result.transform.values[3] *= 0.1f;
                result.transform.values[7] *= 0.1f;
                result.transform.values[11] *= 0.1f;

                _rawTransform = result.transform;
                decomposeTransform(result.transform);
                _fitnessScore = result.fitness_score;
                pc::logger()->info(
                    "AlignmentController: refinement converged, fitness={:.4f}",
                    result.fitness_score);
              } else {
                pc::logger()->warn(
                    "AlignmentController: refinement did not converge");
              }
            } catch (const std::exception &e) {
              pc::logger()->error("AlignmentController::refine: {}", e.what());
            }
            _refining = false;
            emit refiningChanged();
            emit resultChanged();
          });
}

QString AlignmentController::transformString() const {
  QStringList rows;
  for (int r = 0; r < 4; ++r) {
    QStringList cells;
    for (int c = 0; c < 4; ++c) {
      auto v = static_cast<double>(_transform(r, c));
      if (c == 3 && r < 3) v *= 0.1;
      cells << QString("%1").arg(v, 10, 'f', 3);
    }
    rows << cells.join("  ");
  }
  return rows.join("\n");
}

void AlignmentController::snapshotClouds(QObject *primaryAdapter,
                                         QObject *secondaryAdapter) {
  std::shared_ptr<pc::PointCloud> primary, secondary;
  std::shared_ptr<std::vector<std::byte>> primaryRender, secondaryRender;

  QMetaObject::invokeMethod(
      primaryAdapter, "point_cloud",
      Q_RETURN_ARG(std::shared_ptr<pc::PointCloud>, primary));
  QMetaObject::invokeMethod(
      secondaryAdapter, "point_cloud",
      Q_RETURN_ARG(std::shared_ptr<pc::PointCloud>, secondary));
  QMetaObject::invokeMethod(
      primaryAdapter, "render_data",
      Q_RETURN_ARG(std::shared_ptr<std::vector<std::byte>>, primaryRender));
  QMetaObject::invokeMethod(
      secondaryAdapter, "render_data",
      Q_RETURN_ARG(std::shared_ptr<std::vector<std::byte>>, secondaryRender));

  // TODO do we need this deep copy that avoids the shared ptr ??

  _primarySnapshot =
      primary ? std::make_shared<pc::PointCloud>(*primary) : nullptr;
  _secondarySnapshot =
      secondary ? std::make_shared<pc::PointCloud>(*secondary) : nullptr;

  if (primaryRender && !primaryRender->empty()) {
    _primaryRenderData =
        QByteArray(reinterpret_cast<const char *>(primaryRender->data()),
                   static_cast<qsizetype>(primaryRender->size()));
    _primaryRenderStride = static_cast<int>(pc::render_vertex_stride(*primary));
    const auto &b = primary->bounds;
    _primaryBoundsMin = QVector3D(b.min.x, b.min.y, b.min.z);
    _primaryBoundsMax = QVector3D(b.max.x, b.max.y, b.max.z);
  }

  if (secondaryRender && !secondaryRender->empty()) {
    _secondaryRenderData =
        QByteArray(reinterpret_cast<const char *>(secondaryRender->data()),
                   static_cast<qsizetype>(secondaryRender->size()));
    _secondaryRenderStride =
        static_cast<int>(pc::render_vertex_stride(*secondary));
    const auto &b = secondary->bounds;
    _secondaryBoundsMin = QVector3D(b.min.x, b.min.y, b.min.z);
    _secondaryBoundsMax = QVector3D(b.max.x, b.max.y, b.max.z);
  }

  emit snapshotChanged();
}

void AlignmentController::computeFromPairs(const QVariantList &pairs) {
  if (pairs.size() < 3) {
    pc::logger()->error("AlignmentController: need >= 3 pairs");
    return;
  }
  std::vector<pc::float3> source;
  std::vector<pc::float3> target;
  source.reserve(static_cast<std::size_t>(pairs.size()));
  target.reserve(static_cast<std::size_t>(pairs.size()));
  for (const auto &pair : pairs) {
    const auto map = pair.toMap();
    const auto secondary = map["secondary"].value<QVector3D>();
    const auto primary = map["primary"].value<QVector3D>();
    source.push_back(pc::float3{secondary.x(), secondary.y(), secondary.z()});
    target.push_back(pc::float3{primary.x(), primary.y(), primary.z()});
  }
  try {
    const auto transform =
        pc::registration::compute_rigid_transform(source, target);
    _rawTransform = transform;
    decomposeTransform(transform);
    _hasResult = true;
    pc::logger()->info("AlignmentController: computed transform from {} pairs",
                       pairs.size());
    emit resultChanged();
  } catch (const std::exception &e) {
    pc::logger()->error("AlignmentController: {}", e.what());
  }
}

void AlignmentController::refine() {
  if (!_hasResult || _refining) return;
  if (!_primarySnapshot || !_secondarySnapshot || _primarySnapshot->empty() ||
      _secondarySnapshot->empty()) {
    pc::logger()->error("AlignmentController::refine: no snapshot data");
    return;
  }

  _refining = true;
  emit refiningChanged();

  auto params = _refinementParams;
  auto current_transform = _rawTransform;

  // ********
  // TODO is this necessary?
  // Coarse transform was computed in scene-graph units (*0.1),
  // but the snapshot positions are in raw internal units (mm).

  current_transform.values[3] *= 10.0f;  // row 0, col 3
  current_transform.values[7] *= 10.0f;  // row 1, col 3
  current_transform.values[11] *= 10.0f; // row 2, col 3

  auto source = _secondarySnapshot;
  auto target = _primarySnapshot;

  auto future =
      QtConcurrent::run([source, target, params, current_transform]() {
        return pc::registration::refine_alignment(*source, *target,
                                                  current_transform, params);
      });

  _refinementWatcher.setFuture(future);
}

void AlignmentController::decomposeTransform(const pc::float4x4 &matrix) {
  const auto [position, rotation] = pc::decompose_transform(matrix);
  _transform = QMatrix4x4(matrix.values.data());
  _position = QVector3D(position.x, position.y, position.z);
  _rotation = QQuaternion(rotation.scalar, rotation.x, rotation.y, rotation.z);
}

void AlignmentController::reset() {
  _hasResult = false;
  _transform = QMatrix4x4();
  _position = QVector3D();
  _rotation = QQuaternion();
  _fitnessScore = 0.0f;
  emit resultChanged();
}

} // namespace pc::ui::qml