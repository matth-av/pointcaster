#pragma once

#include "attribute_list_model.h"
#include "camera_image_provider.h"
#include "config_adapter.h"
#include "enum_adapters.h"
#include "operator_adapter.h"
#include "point_cloud_adapter.h"

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <cstring>
#include <memory>
#include <plugins/devices/device_plugin.h>
#include <plugins/devices/device_variants.h>
#include <pointcaster/point_cloud.h>
#include <qtmetamacros.h>
#include <variant>

class DeviceAdapter : public ConfigAdapter, public PointCloudAdapter {
  Q_OBJECT

  Q_PROPERTY(
      pc::ui::WorkspaceDeviceStatus status READ status NOTIFY statusChanged)

  Q_PROPERTY(QList<OperatorAdapter *> operatorAdapters READ operatorAdapters
                 NOTIFY operatorAdaptersChanged)

  Q_PROPERTY(QStringList frameSlots READ frameSlots NOTIFY frameSlotsChanged)
  Q_PROPERTY(QVariantMap frameUrls READ frameUrls NOTIFY frameUrlsChanged)

  Q_PROPERTY(bool hasSequence READ hasSequence NOTIFY sequenceStateChanged)
  Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY sequenceStateChanged)
  Q_PROPERTY(int frameCount READ frameCount NOTIFY sequenceStateChanged)
  Q_PROPERTY(int currentFrame READ currentFrame NOTIFY sequenceStateChanged)

  Q_PROPERTY(
      bool pluginNullState READ pluginNullState NOTIFY pluginNullStateChanged)

  // empty for a device whose source declares no per-point attributes
  Q_PROPERTY(QAbstractListModel *attributeList READ attributeList CONSTANT)

public:
  explicit DeviceAdapter(pc::devices::DevicePlugin *plugin,
                         CameraImageProvider *imageProvider,
                         QObject *parent = nullptr)
      : ConfigAdapter(parent), _plugin(plugin), _imageProvider(imageProvider) {
    // a device that loaded before its adapter existed already has attributes
    // to describe
    refreshAttributeList();
  }

  ~DeviceAdapter() override {
    if (_imageProvider) _imageProvider->removeFrames(deviceKeyPrefix());
  }

  using ConfigAdapter::setConfig;

  virtual bool setConfig(const pc::devices::DeviceConfigurationVariant &) = 0;

  bool setConfig(const pc::ConfigurationVariant &) override { return false; }

  QList<OperatorAdapter *> operatorAdapters() const {
    return _operatorAdapters;
  }

  QAbstractListModel *attributeList() { return &_attributeList; }

  void refreshAttributeList() { _attributeList.bind(this, _plugin); }

  void rebuildOperatorAdapters() {
    qDeleteAll(_operatorAdapters);
    _operatorAdapters.clear();
    if (!_plugin) return;
    for (auto &op : _plugin->operators) {
      auto *adapter = new OperatorAdapter(op.get(), _plugin, this);
      _operatorAdapters.append(adapter);
    }
    emit operatorAdaptersChanged();
  }

  // --- camera frame visualisation

  QStringList frameSlots() const { return _frameSlotNames; }
  QVariantMap frameUrls() const { return _frameUrls; }

  void syncCameraFrames() {
    if (!_plugin || !_imageProvider) return;

    auto frames = _plugin->latest_camera_frames();

    // Operator removed or no frames yet: clear slots if we had some.
    if (frames.empty()) {
      if (!_frameSlotNames.isEmpty()) {
        _imageProvider->removeFrames(deviceKeyPrefix());
        _frameSlotNames.clear();
        _frameUrls.clear();
        emit frameSlotsChanged();
        emit frameUrlsChanged();
      }
      return;
    }

    const auto prefix = deviceKeyPrefix();

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

  // ----------------- identity -----------------
  int deviceIndex() const { return _deviceIndex; }
  void setDeviceIndex(int index) { _deviceIndex = index; }

  QString deviceId() const { return _deviceId; }
  void setDeviceId(const QString &id) { _deviceId = id; }

  void invalidatePlugin() {
    _plugin = nullptr;
    for (auto *op : _operatorAdapters) {
      if (op) op->invalidatePlugin();
    }
    refreshAttributeList();
  }

  bool pluginNullState() const {
    if (!_plugin) return false;
    return _plugin->plugin_null_state();
  }

  pc::devices::DevicePlugin *plugin() const { return _plugin; }

  // ----------------- status -----------------
  pc::ui::WorkspaceDeviceStatus status() const { return _status; }

  void setStatusFromCore(pc::devices::DeviceStatus s) {
    const auto q = pc::ui::toQt(s);
    if (q == _status) return;
    _status = q;
    // a device that just finished loading has attributes to describe now
    refreshAttributeList();
    emit statusChanged();
  }

  Q_INVOKABLE void start() {
    if (!_plugin) return;
    _plugin->start();
  }

  Q_INVOKABLE void stop() {
    if (!_plugin) return;
    _plugin->stop();
  }

  Q_INVOKABLE void restart() {
    if (!_plugin) return;
    pc::logger()->trace("Running restart from the device adapter i.e. qml");
    _plugin->restart();
  }

  Q_INVOKABLE std::shared_ptr<pc::PointCloud> point_cloud() override {
    if (!_plugin) return {};
    return _plugin->point_cloud();
  };

  Q_INVOKABLE std::shared_ptr<std::vector<std::byte>> render_data() override {
    if (!_plugin) return {};
    return _plugin->render_data();
  }

  void notifyFieldChanged(const QString &path) override {
    emit fieldChanged(path);
    if (_plugin && !isRefreshingAllFields()) {
      _plugin->on_config_field_changed(path.toStdString());
    }
    if (path.contains(QStringLiteral("file")) ||
        path.contains(QStringLiteral("attributes"))) {
      refreshAttributeList();
    }
  }

  void notifyPointCloudUpdated() {
    emit pointCloudUpdated();
    syncCameraFrames();
    if (!_plugin) return;

    int frame = 0;
    std::visit(
        [&](const auto &config) {
          if constexpr (requires { config.sequence.current_frame; })
            frame = config.sequence.current_frame;
        },
        _plugin->config());
    updateSequenceState(frame);
  }

  bool hasSequence() const { return _plugin && _plugin->is_sequence(); }
  bool isPlaying() const { return _isPlaying; }

  int frameCount() const {
    return _plugin ? static_cast<int>(_plugin->frame_count()) : 1;
  }

  int currentFrame() const { return _currentFrame; }

  void updateSequenceState(int frame) {
    if (!_plugin) return;
    bool playing = false;
    std::visit(
        [&](const auto &config) {
          if constexpr (requires { config.sequence.playing; })
            playing = config.sequence.playing;
        },
        _plugin->config());

    const bool seqChanged =
        (_plugin && _plugin->is_sequence()) != _lastHasSequence;
    const bool frameCountChanged =
        (_plugin ? static_cast<int>(_plugin->frame_count()) : 1) !=
        _lastFrameCount;
    const bool frameChanged = frame != _currentFrame;
    const bool playingChanged = playing != _isPlaying;

    _currentFrame = frame;
    _isPlaying = playing;
    _lastHasSequence = _plugin && _plugin->is_sequence();
    _lastFrameCount = _plugin ? static_cast<int>(_plugin->frame_count()) : 1;

    if (seqChanged || frameCountChanged || frameChanged || playingChanged)
      emit sequenceStateChanged();
  }

signals:
  void statusChanged();
  void pluginNullStateChanged();
  void pointCloudUpdated();
  void operatorAdaptersChanged();
  void frameSlotsChanged();
  void frameUrlsChanged();
  void sequenceStateChanged();

protected:
  pc::devices::DevicePlugin *_plugin = nullptr;
  CameraImageProvider *_imageProvider = nullptr;
  pc::ui::WorkspaceDeviceStatus _status =
      pc::ui::WorkspaceDeviceStatus::Unloaded;

  int _deviceIndex = -1;
  QString _deviceId;

  QList<OperatorAdapter *> _operatorAdapters;

  pc::ui::AttributeListModel _attributeList{this};

  QStringList _frameSlotNames;
  QVariantMap _frameUrls;
  int _frameRevision = 0;

  int _currentFrame = 0;
  bool _isPlaying = false;
  bool _lastHasSequence = false;
  int _lastFrameCount = 1;

private:
  QString deviceKeyPrefix() const {
    if (!_deviceId.isEmpty()) return _deviceId + "/";
    if (!_plugin) return {};
    QString id;
    std::visit(
        [&](const auto &config) { id = QString::fromStdString(config.id); },
        _plugin->config());
    return id + "/";
  }
};