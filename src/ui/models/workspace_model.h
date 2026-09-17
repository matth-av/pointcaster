#pragma once

#include "config_adapter.h"
#include "device_adapter.h"
#include "enum_adapters.h"
#include "log_model.h"
#include "operator_adapter.h"
#include "session_adapter.h"
#include "session_recorder_model.h"
#include "stream_channel_model.h"
#include "stream_source.h"
#include <QMatrix4x4>
#include <QObject>
#include <QPointer>
#include <QUndoStack>
#include <QUrl>
#include <QVariant>
#include <config/config_registry.h>
#include <config/config_variant.h>
#include <functional>
#include <optional>
#include <point_streamer/point_streamer_config_adapter.gen.h>
#include <publishers/publishers_config_adapter.gen.h>
#include <qtmetamacros.h>
#include <session/session_config_adapter.gen.h>
#include <string>
#include <utility>
#include <vector>
#include <workspace/workspace_config.h>

namespace pc {
class Workspace;
class Session;
struct SessionConfiguration;

namespace devices {
class DevicePlugin;
class DeviceGroupConfigurationAdapter;
} // namespace devices

} // namespace pc

class CameraImageProvider;

namespace pc::ui {

class WorkspaceModel : public QObject {
  Q_OBJECT

  Q_PROPERTY(QUndoStack *undoStack READ undoStack CONSTANT)
  Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)

  // Sessions
  Q_PROPERTY(QList<QObject *> sessionAdapters READ sessionAdapters NOTIFY
                 sessionAdaptersChanged)
  Q_PROPERTY(QString selectedSessionId READ selectedSessionId WRITE
                 setSelectedSessionId NOTIFY selectedSessionChanged)
  Q_PROPERTY(QObject *selectedSessionAdapter READ selectedSessionAdapter NOTIFY
                 selectedSessionChanged)
  Q_PROPERTY(QList<OperatorAdapter *> selectedSessionOperatorAdapters READ
                 selectedSessionOperatorAdapters NOTIFY selectedSessionChanged)

  // Devices
  Q_PROPERTY(
      QVariant deviceAdapters READ deviceAdapters NOTIFY deviceAdaptersChanged)
  Q_PROPERTY(QStringList deviceVariantNames READ deviceVariantNames NOTIFY
                 deviceVariantNamesChanged)
  Q_PROPERTY(QVariantList addDeviceMenuEntries READ addDeviceMenuEntries NOTIFY
                 addDeviceMenuEntriesChanged)
  Q_PROPERTY(int selectedDeviceIndex READ selectedDeviceIndex WRITE
                 setSelectedDeviceIndex NOTIFY selectedDeviceIndexChanged)
  Q_PROPERTY(QVariantList deviceTreeRows READ deviceTreeRows NOTIFY
                 deviceTreeRowsChanged)
  Q_PROPERTY(
      QString selectedNodeId READ selectedNodeId NOTIFY selectedNodeChanged)
  Q_PROPERTY(
      QString selectedNodeKind READ selectedNodeKind NOTIFY selectedNodeChanged)

  Q_PROPERTY(QObject *selectedDeviceGroupAdapter READ selectedDeviceGroupAdapter
                 NOTIFY selectedDeviceGroupAdapterChanged)

  Q_PROPERTY(bool selectedGroupHasSequence READ selectedGroupHasSequence NOTIFY
                 selectedGroupHasSequenceChanged)

  // config registry paths that leave the machine, bindable so a field row can
  // show its own state
  Q_PROPERTY(
      QStringList publishPaths READ publishPaths NOTIFY publishPathsChanged)
  Q_PROPERTY(QStringList pushPaths READ pushPaths NOTIFY pushPathsChanged)

  // and the output stream paths the 3d scene draws
  Q_PROPERTY(QStringList renderPaths READ renderPaths NOTIFY renderPathsChanged)

  // Selected operator (owned by a device or a session)
  Q_PROPERTY(OperatorAdapter *selectedOperatorAdapter READ
                 selectedOperatorAdapter WRITE setSelectedOperatorAdapter NOTIFY
                     selectedOperatorAdapterChanged)

  // The host of the selected operator's operator pipeline...
  // either a device or a session
  Q_PROPERTY(
      QObject *selectedOperatorFrameSource READ selectedOperatorFrameSource
          NOTIFY selectedOperatorFrameSourceChanged)

  Q_PROPERTY(QUrl saveFileUrl READ saveFileUrl WRITE setSaveFileUrl NOTIFY
                 saveFileUrlChanged)

  // Console logger window
  Q_PROPERTY(LogModel *logModel READ logModel CONSTANT)

  Q_PROPERTY(QVariantMap foldedPropertyPaths READ foldedPropertyPaths NOTIFY
                 foldedPropertyPathsChanged)

  Q_PROPERTY(QVariantMap uiState READ uiState NOTIFY uiStateChanged)

  Q_PROPERTY(QVariantList availableOperators READ availableOperators CONSTANT)

  // Streaming
  Q_PROPERTY(QObject *pointStreamerAdapter READ pointStreamerAdapter CONSTANT)
  Q_PROPERTY(QAbstractListModel *streamChannels READ streamChannels CONSTANT)
  Q_PROPERTY(int selectedStreamChannelIndex READ selectedStreamChannelIndex
                 WRITE setSelectedStreamChannelIndex NOTIFY
                     selectedStreamChannelIndexChanged)

  // Publishers
  Q_PROPERTY(
      QObject *publishersConfigAdapter READ publishersConfigAdapter CONSTANT)

  // Recording
  Q_PROPERTY(RecorderModel *recorder READ recorder NOTIFY recorderChanged)

public:
  explicit WorkspaceModel(pc::Workspace *workspace, QObject *parent);
  ~WorkspaceModel();
  Q_DISABLE_COPY_MOVE(WorkspaceModel)

  Q_INVOKABLE void close();
  Q_INVOKABLE void loadFromFile(const QUrl &file);
  Q_INVOKABLE void save(bool update_last_session_path = true);

  Q_INVOKABLE void newWorkspace();

  Q_INVOKABLE void registerPluginSettingsPages();

  QUndoStack *undoStack() const { return _undoStack; }
  bool isDirty() const { return !_undoStack->isClean(); }

  // config registry subscriptions owned by this model. subscribeToRegistry
  // records what we registered so unsubscribeFromRegistry can drop our own
  // subscriptions under an area without touching anybody else's
  pc::ConfigRegistry::SubscriptionId
  subscribeToRegistry(std::string prefix,
                      pc::ConfigRegistry::ChangeCallback cb);
  void unsubscribeFromRegistry(std::string_view area);

  Q_INVOKABLE void triggerDeviceDiscovery();
  Q_INVOKABLE void addNewDevice(const QString &plugin_name,
                                const QString &target_ip = "",
                                const QString &target_id = "",
                                const QString &target_type_label = "");
  Q_INVOKABLE void deleteDevice(const QString &target_id);
  Q_INVOKABLE void deleteSelectedDeviceNode();
  Q_INVOKABLE void duplicateDeviceNode(const QString &node_id);
  Q_INVOKABLE void duplicateSelectedDeviceNode();
  Q_INVOKABLE void setDeviceLabel(const QString &device_id,
                                  const QString &new_label);

  QList<QObject *> sessionAdapters() const;
  Q_INVOKABLE QList<QObject *> sessionAdaptersCallable() const {
    return sessionAdapters();
  }

  QString selectedSessionId() const { return _selectedSessionId; }
  void setSelectedSessionId(const QString &id);
  QObject *selectedSessionAdapter() const;
  QList<OperatorAdapter *> selectedSessionOperatorAdapters() const;

  Q_INVOKABLE void addOperatorToSession(const QString &sessionId,
                                        const QString &operatorPluginName);
  Q_INVOKABLE void removeOperatorFromSession(const QString &sessionId,
                                             int operatorIndex);
  Q_INVOKABLE void reorderOperatorOnSession(const QString &sessionId,
                                            int fromIndex, int toIndex);

  Q_INVOKABLE SessionAdapter *
  sessionPointCloudAdapterFor(const QString &sessionId) const;

  Q_INVOKABLE QList<OperatorAdapter *>
  sessionOperatorAdaptersFor(const QString &sessionId) const;

  Q_INVOKABLE QString addSession();
  Q_INVOKABLE void removeSession(const QString &sessionId);
  Q_INVOKABLE void duplicateSession(const QString &sessionId);
  Q_INVOKABLE void setSessionLabel(const QString &sessionId,
                                   const QString &label);
  Q_INVOKABLE QString sessionIdForLabel(const QString &label) const;

  // whether a device or group node is switched on for the selected session
  Q_INVOKABLE bool sessionDeviceEnabled(const QString &node_id) const;
  Q_INVOKABLE void setSessionDeviceEnabled(const QString &node_id,
                                           bool enabled);

  // whether one session's view draws a rendered output path
  Q_INVOKABLE bool sessionDrawsRenderPath(const QString &sessionId,
                                          const QString &path) const;

  QVariant deviceAdapters() const;

  QStringList deviceVariantNames() const;
  QVariantList addDeviceMenuEntries() const;

  int selectedDeviceIndex() const { return _selectedDeviceIndex; }
  void setSelectedDeviceIndex(int index);

  Q_INVOKABLE DeviceAdapter *deviceAdapterAt(int index) const;
  Q_INVOKABLE DeviceAdapter *selectedDeviceAdapter() const {
    return deviceAdapterAt(_selectedDeviceIndex);
  }

  QVariantList deviceTreeRows() const;

  QString selectedNodeId() const { return _selectedNodeId; }
  QString selectedNodeKind() const { return _selectedNodeKind; }
  Q_INVOKABLE void selectNode(const QString &node_id);

  QObject *selectedDeviceGroupAdapter() const;

  bool selectedGroupHasSequence() const { return _selectedGroupHasSequence; }

  Q_INVOKABLE QMatrix4x4 nodeAncestorWorldMatrix(const QString &node_id) const;

  Q_INVOKABLE QVariantMap localTransformForWorldAlignment(
      const QString &device_id, const QMatrix4x4 &worldAlignment) const;

  Q_INVOKABLE void moveDeviceNode(const QString &node_id,
                                  const QString &new_parent_id,
                                  const QString &before_node_id = "");

  Q_INVOKABLE void createDeviceGroup(const QString &parent_id = "");
  Q_INVOKABLE bool deleteDeviceGroup(const QString &group_id);
  Q_INVOKABLE void setDeviceGroupCollapsed(const QString &group_id,
                                           bool collapsed);
  Q_INVOKABLE void setDeviceGroupLabel(const QString &group_id,
                                       const QString &label);

  OperatorAdapter *selectedOperatorAdapter() const {
    return _selectedOperatorAdapter;
  }
  void setSelectedOperatorAdapter(OperatorAdapter *adapter);

  QObject *selectedOperatorFrameSource() const;

  Q_INVOKABLE void addOperatorToDevice(int deviceIndex,
                                       const QString &operatorPluginName);
  Q_INVOKABLE void removeOperatorFromDevice(int deviceIndex, int operatorIndex);
  Q_INVOKABLE void reorderOperatorOnDevice(int deviceIndex, int fromIndex,
                                           int toIndex);

  Q_INVOKABLE void attachOperatorConfigAdapters(DeviceAdapter *deviceAdapter);
  Q_INVOKABLE void initOperatorAdapter(OperatorAdapter *opAdapter,
                                       DeviceAdapter *deviceAdapter);

  QUrl saveFileUrl() const { return _saveFileUrl; }
  void setSaveFileUrl(const QUrl &url) {
    if (_saveFileUrl == url) return;
    _saveFileUrl = url;
    emit saveFileUrlChanged();
  }

  LogModel *logModel() const { return _logModel; }

  QObject *pointStreamerAdapter() const { return _pointStreamerAdapter.data(); }

  QObject *publishersConfigAdapter() const {
    return _publishersConfigAdapter.data();
  }

  QAbstractListModel *streamChannels() const { return _streamChannelModel; }

  int selectedStreamChannelIndex() const { return _selectedStreamChannelIndex; }
  void setSelectedStreamChannelIndex(int index);

  RecorderModel *recorder() const { return _recorderModel; }

  // each entry is e.g. { name: "ClusterExtractionOperator", label: "Cluster
  // Extraction" }
  Q_INVOKABLE QVariantList availableOperators() const;

  Q_INVOKABLE QVariantMap foldedPropertyPaths() const {
    return _foldedPropertyPaths;
  }

  Q_INVOKABLE void setFoldedProperty(const QString &path, bool folded) {
    _foldedPropertyPaths[path] = folded;
    foldedPropertyPathsChanged();
  }

  Q_INVOKABLE QVariantMap uiState() const { return _uiState; }

  Q_INVOKABLE void setUiState(const QVariantMap &state) {
    _uiState = state;
    emit uiStateChanged();
  }

  Q_INVOKABLE void setUiStateValue(const QString &key, const QVariant &value) {
    _uiState[key] = value;
    emit uiStateChanged();
  }

  void setImageProvider(CameraImageProvider *provider);

  QStringList publishPaths() const;
  QStringList pushPaths() const;
  QStringList renderPaths() const;

  Q_INVOKABLE void addPublishPath(const QString &path);
  Q_INVOKABLE void removePublishPath(const QString &path);

  Q_INVOKABLE void addPushPath(const QString &path);
  Q_INVOKABLE void removePushPath(const QString &path);

  Q_INVOKABLE void addRenderPath(const QString &path);
  Q_INVOKABLE void removeRenderPath(const QString &path);

  Q_INVOKABLE StreamSource *streamSourceFor(const QString &path);

  // the world transform of the space the value at a rendered path sits in
  Q_INVOKABLE QMatrix4x4 renderPathParentWorld(const QString &path) const;

  Q_INVOKABLE QMatrix4x4 renderPathOwnerWorld(const QString &path) const;

  Q_INVOKABLE QString renderPathOwnerId(const QString &path) const;

  Q_INVOKABLE QMatrix4x4 nodeWorldMatrix(const QString &node_id) const;

public slots:
  void syncAdapters();
  void syncSessionAdapters();
  void syncDeviceAdapters();

signals:
  void openSaveAsDialog();

  void newWorkspaceLoaded();

  void sessionAdaptersChanged();
  void selectedSessionChanged();
  void sessionAdded(const QString &sessionId);
  void sessionOperatorsChanged(const QString &sessionId);

  void deviceAdaptersChanged();
  void deviceVariantNamesChanged();
  void addDeviceMenuEntriesChanged();
  void selectedDeviceIndexChanged();

  void selectedNodeChanged();

  void selectedDeviceGroupAdapterChanged();

  void selectedGroupHasSequenceChanged();

  void deviceAdded();
  void deviceDeleted();

  void deviceTreeRowsChanged();

  void publishPathsChanged();
  void pushPathsChanged();
  void renderPathsChanged();

  void selectedOperatorAdapterChanged();
  void selectedOperatorFrameSourceChanged();

  void foldedPropertyPathsChanged();

  void saveFileUrlChanged();

  void dirtyChanged();

  void uiStateChanged();

  void recorderChanged();

  void selectedStreamChannelIndexChanged();

private:
  // a preview of session data before its written...
  // for when in the middle of transforming something
  // using a gizmo but haven't yet 'commited' it into the workspace for undo
  // etc.
  template <typename ConfigT> struct PreviewSession {
    QString id;
    ConfigT before;
    pc::WorkspaceConfiguration base_snapshot;
    // whether any preview actually changed something
    bool dirty = false;
  };

  std::optional<PreviewSession<pc::devices::DeviceConfigurationVariant>>
      _devicePreview;
  std::optional<PreviewSession<pc::devices::DeviceGroupConfiguration>>
      _groupPreview;

  // a gizmo or a drag on an operator's field previews through these
  std::optional<PreviewSession<pc::operators::OperatorConfigurationVariant>>
      _deviceOperatorPreview;
  std::optional<PreviewSession<pc::SessionConfiguration>>
      _sessionOperatorPreview;

  pc::Workspace &_workspace;

  CameraImageProvider *_imageProvider = nullptr;

  // TODO raw pointer and new? really?
  std::vector<std::pair<pc::ConfigRegistry::SubscriptionId, std::string>>
      _registrySubscriptions;

  QUndoStack *_undoStack = new QUndoStack(this);

  QList<QObject *> _sessionAdapters;
  QList<QObject *> _deviceAdapters;

  QHash<QString, QPointer<SessionAdapter>> _sessionPointCloudAdapters;

  // one per path the scene has been asked to draw
  QHash<QString, StreamSource *> _streamSources;

  QPointer<ConfigAdapter> _selectedDeviceGroupAdapter;
  bool _selectedGroupHasSequence = false;

  QPointer<OperatorAdapter> _selectedOperatorAdapter;

  QPointer<ConfigAdapter> _pointStreamerAdapter;

  QPointer<ConfigAdapter> _publishersConfigAdapter;

  StreamChannelListModel *_streamChannelModel;
  int _selectedStreamChannelIndex = -1;

  LogModel *_logModel;

  RecorderModel *_recorderModel;

  // Tracks whether an existing SessionConfigurationAdapter is still bound to
  // a valid underlying SessionConfiguration object address.
  QHash<QString, const pc::SessionConfiguration *> _sessionConfigPtrById;

  QHash<QString, QPointer<pc::SessionConfigurationAdapter>> _sessionAdapterById;
  QHash<QString, QString> _sessionLabelById;

  // Currently selected (focused) session, and the per-session operator
  // adapters wrapping each session's live OperatorPlugin instances.
  QString _selectedSessionId;
  QHash<QString, QList<OperatorAdapter *>> _sessionOperatorAdapters;

  QVariantMap _foldedPropertyPaths;
  QVariantMap _uiState;

  int _selectedDeviceIndex = 0;
  QString _selectedNodeId;
  QString _selectedNodeKind; // "device" | "group" | ""

  QUrl _saveFileUrl;

  // applies a new config and syncs adapters on the UI thread
  enum class RebuildScope { None, Sessions, Devices, All };
  void applyWorkspaceConfigAndRebuild(pc::WorkspaceConfiguration new_config,
                                      RebuildScope scope = RebuildScope::All);

  // Helpers to get stable ids from adapters without relying on Q_PROPERTY
  // names.
  static QString adapterStableId(ConfigAdapter *adapter);
  static QString adapterStableId(DeviceAdapter *adapter);

  void initDeviceAdapter(auto *adapter, pc::devices::DevicePlugin *plugin);
  void initSessionAdapter(pc::SessionConfigurationAdapter *adapter);

  void syncSessionPointCloudAdapters();

  // Session operator adapters (analogous to attach/init for devices).
  void attachSessionOperatorConfigAdapters(const QString &sessionId,
                                           const QList<OperatorAdapter *> &ops);
  void initSessionOperatorAdapter(OperatorAdapter *opAdapter,
                                  const QString &sessionId);
  void rebuildSessionOperatorAdapters(const QString &sessionId,
                                      pc::SessionConfigurationAdapter *adapter,
                                      pc::Session *session);
  void syncSessionOperatorAdapters();

  void rebuildSelectedGroupAdapter();
  // recomputes _selectedGroupHasSequence; a plain member (not a lambda) so the
  // per-device sequenceStateChanged hookups can use Qt::UniqueConnection
  void refreshSelectedGroupHasSequence();
  void initGroupAdapter(pc::devices::DeviceGroupConfigurationAdapter *adapter);
  void retransformGroupDescendants(const std::string &group_id);

  // a group's transport buttons control every child sequence device
  void propagateGroupTransport(const std::string &group_id, const QString &path,
                               const QVariant &value);

  DeviceAdapter *makeDeviceAdapterForPlugin(
      pc::devices::DevicePlugin *plugin,
      pc::devices::DeviceConfigurationVariant &config_variant);

  bool isDescendantOf(const std::string &node_id,
                      const std::string &maybe_ancestor_id) const;

  void refreshOperatorConfigAdapters();
  void notifyOperatorConfigAdapters(const QList<OperatorAdapter *> &ops);

  // keeps whatever the scene draws from an adapter's paths in step with edits
  // to them
  void watchStreamSourcesFor(ConfigAdapter *adapter);

  void initPointStreamerAdapter();
  void initPublishersConfigAdapter();
};

} // namespace pc::ui