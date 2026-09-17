#pragma once

#include "camera_image_provider.h"
#include "point_cloud_adapter.h"

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <session/session.h>

class SessionAdapter : public QObject, public PointCloudAdapter {
  Q_OBJECT

  Q_PROPERTY(QStringList frameSlots READ frameSlots NOTIFY frameSlotsChanged)
  Q_PROPERTY(QVariantMap frameUrls READ frameUrls NOTIFY frameUrlsChanged)

  // ---- session-wide playback 
  Q_PROPERTY(bool hasSequence READ hasSequence NOTIFY playbackChanged)
  Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playbackChanged)
  Q_PROPERTY(int currentFrame READ currentFrame NOTIFY playbackChanged)
  Q_PROPERTY(int totalFrames READ totalFrames NOTIFY playbackChanged)
  Q_PROPERTY(int loopIn READ loopIn NOTIFY playbackChanged)
  Q_PROPERTY(int loopOut READ loopOut NOTIFY playbackChanged)
  Q_PROPERTY(bool looping READ looping NOTIFY playbackChanged)

public:
  explicit SessionAdapter(pc::Session *session,
                          CameraImageProvider *imageProvider,
                          QObject *parent = nullptr)
      : QObject(parent), _session(session), _imageProvider(imageProvider) {}

  ~SessionAdapter() override {
    if (_imageProvider) _imageProvider->removeFrames(keyPrefix());
  }

  // ---- PointCloudAdapter ----
  Q_INVOKABLE std::shared_ptr<pc::PointCloud> point_cloud() override {
    return _session ? _session->point_cloud() : nullptr;
  }
  Q_INVOKABLE std::shared_ptr<std::vector<std::byte>> render_data() override {
    return _session ? _session->render_data() : nullptr;
  }

  void invalidateSession() { _session = nullptr; }

  // ---- camera frame slots / URLs ----
  QStringList frameSlots() const { return _frameSlotNames; }
  QVariantMap frameUrls() const { return _frameUrls; }

  // ---- playback reads (delegate to core) ----
  bool hasSequence() const {
    return _session && _session->playback_has_sequence();
  }
  bool isPlaying() const { return _session && _session->playback_is_playing(); }
  int currentFrame() const {
    return _session ? _session->playback_current_frame() : 0;
  }
  int totalFrames() const {
    return _session ? _session->playback_total_frames() : 1;
  }
  int loopIn() const { return _session ? _session->playback_loop_in() : 0; }
  int loopOut() const { return _session ? _session->playback_loop_out() : 0; }
  bool looping() const { return _session && _session->playback_looping(); }

  // ---- playback commands (forward to core) ----
  Q_INVOKABLE void play() {
    if (_session) _session->playback_play();
  }
  Q_INVOKABLE void pause() {
    if (_session) _session->playback_pause();
  }
  Q_INVOKABLE void stop() {
    if (_session) _session->playback_stop();
  }
  Q_INVOKABLE void scrub(int frame) {
    if (_session) _session->playback_scrub(frame);
  }
  Q_INVOKABLE void setLoop(int in, int out) {
    if (_session) _session->playback_set_loop(in, out);
  }
  Q_INVOKABLE void setLooping(bool v) {
    if (_session) _session->playback_set_looping(v);
  }
  Q_INVOKABLE void setFps(int fps) {
    if (_session) _session->playback_set_fps(fps);
  }

  // ---- callbacks (invoked on the UI thread by WorkspaceModel) ----
  void notifyPointCloudUpdated() {
    emit pointCloudUpdated();
    syncCameraFrames();
  }
  void notifyPlaybackChanged() { emit playbackChanged(); }

  void syncCameraFrames() {
    if (!_session || !_imageProvider) return;

    auto frames = _session->latest_camera_frames();
    if (frames.empty()) {
      if (!_frameSlotNames.isEmpty()) {
        _imageProvider->removeFrames(keyPrefix());
        _frameSlotNames.clear();
        _frameUrls.clear();
        emit frameSlotsChanged();
        emit frameUrlsChanged();
      }
      return;
    }

    const auto prefix = keyPrefix();
    QStringList newSlotNames;
    QVariantMap newUrls;
    for (const auto &frame : frames) {
      if (!frame.frame_data) continue;
      auto &data = *frame.frame_data.value();

      const auto w = static_cast<int>(data.width);
      const auto h = static_cast<int>(data.height);
      const auto &colors = data.main_color_buffer();

      QImage img(reinterpret_cast<const uchar *>(colors.data()), w, h, w * 4,
                 QImage::Format_RGBA8888);
      img = img.copy();

      const auto name = QString::fromStdString(frame.name);
      const auto key = prefix + name;
      _imageProvider->registerFrame(key, img);
      newSlotNames.append(name);
      newUrls[name] = QStringLiteral("image://camera/") + key + "?" +
                      QString::number(_frameRevision);
    }
    _frameRevision++;

    if (newSlotNames != _frameSlotNames) {
      _frameSlotNames = std::move(newSlotNames);
      emit frameSlotsChanged();
    }
    _frameUrls = std::move(newUrls);
    emit frameUrlsChanged();
  }

  pc::Session *session() const { return _session; }

signals:
  void pointCloudUpdated();
  void frameSlotsChanged();
  void frameUrlsChanged();
  void playbackChanged();

private:
  pc::Session *_session = nullptr;
  CameraImageProvider *_imageProvider = nullptr;

  QStringList _frameSlotNames;
  QVariantMap _frameUrls;
  int _frameRevision = 0;

  QString keyPrefix() const {
    return _session ? QString::fromStdString(_session->id) + "/" : QString{};
  }
};