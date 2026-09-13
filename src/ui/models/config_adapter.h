#pragma once

#include <config/config_variant.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

// pure virtual members in this abstract class are replaced by functions in
// each generated adapter implementation

class ConfigAdapter : public QObject {
  Q_OBJECT

  Q_PROPERTY(
      QVariantList childPathGroups READ childPathGroups NOTIFY structureChanged)

  // the config registry prefix these fields sit under, like
  // "device/group_a/cam_1"
  Q_PROPERTY(QString configPath READ configPath NOTIFY configPathChanged)

public:
  explicit ConfigAdapter(QObject *parent = nullptr) : QObject(parent) {}
  ~ConfigAdapter() override = default;

  Q_INVOKABLE virtual QString configType() const = 0;
  Q_INVOKABLE virtual QString displayName() const { return {}; }

  // stable paths to each member like:
  //  - "id"
  //  - "camera/locked"
  Q_INVOKABLE virtual QStringList fieldPaths() const = 0;

  // lists of paths sorted to child Configuration structs like:
  // [["transform/position", "transform/rotation"],
  //  ["camera/id", "camera/locked"]]
  Q_INVOKABLE virtual QList<QStringList> childPaths() const = 0;

  QVariantList childPathGroups() const {
    QVariantList groups;
    for (const auto &group : childPaths()) {
      groups.append(QVariant::fromValue(group));
    }
    return groups;
  }

  Q_INVOKABLE virtual QString
  parentConfigurationName(const QString &path) const = 0;

  // ---- Path-based value access
  // Path segments use "/" as separator, e.g. "camera/locked".
  Q_INVOKABLE virtual QVariant value(const QString &path) const = 0;

  // UI uses this... WorkspaceModel listens to editRequested() and pushes undo
  // commands
  Q_INVOKABLE void set(const QString &path, const QVariant &value) {
    emit editRequested(path, value);
  }

  // set the preview of the adapter state without
  // committing it to the workspace
  Q_INVOKABLE void setPreview(const QString &path, const QVariant &value) {
    emit previewRequested(path, value);
  }

  // Mutating apply that directly changes the referenced config.
  // Returns true if a change actually happened.
  Q_INVOKABLE virtual bool apply(const QString &path,
                                 const QVariant &value) = 0;

  // ---- defautl metadata is empty
  Q_INVOKABLE virtual QString typeName(const QString &path) const {
    Q_UNUSED(path);
    return {};
  }

  Q_INVOKABLE virtual QVariant defaultValue(const QString &path) const {
    Q_UNUSED(path);
    return {};
  }

  Q_INVOKABLE virtual QVariant minMax(const QString &path) const {
    Q_UNUSED(path);
    return {};
  }

  Q_INVOKABLE virtual bool isOptional(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isDisabled(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isHidden(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isOutput(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isStream(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual QString streamType(const QString &path) const {
    Q_UNUSED(path);
    return {};
  }

  Q_INVOKABLE virtual QStringList radiusPaths() const { return {}; }

  Q_INVOKABLE virtual bool isFoldedByDefault(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isEnum(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isVariant(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual QVariant enumOptions(const QString &path) const {
    Q_UNUSED(path);
    return {};
  }

  Q_INVOKABLE virtual bool isFileOpener(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isFolderOpener(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  Q_INVOKABLE virtual bool isButton(const QString &path) const {
    Q_UNUSED(path);
    return false;
  }

  virtual void notifyFieldChanged(const QString &path) {
    emit fieldChanged(path);
  }

  // a structure change makes the editor tear down and rebuild its field
  // delegates
  void notifyStructureChanged() {
    QMetaObject::invokeMethod(
        this, [this] { emit structureChanged(); }, Qt::QueuedConnection);
  }

  // true while notifyAllFieldsChanged() is walking this adapter's paths...
  bool isRefreshingAllFields() const { return _refreshingAllFields; }

  // re-announces every field so QML re-reads it
  void notifyAllFieldsChanged() {
    _refreshingAllFields = true;
    for (auto *nested :
         findChildren<ConfigAdapter *>(Qt::FindDirectChildrenOnly)) {
      nested->notifyAllFieldsChanged();
    }
    for (const QString &path : fieldPaths()) {
      notifyFieldChanged(path);
    }
    notifyAllPropertiesChanged();
    _refreshingAllFields = false;
  }

  virtual void notifyAllPropertiesChanged() {}

  virtual bool setConfig(const pc::ConfigurationVariant &) { return false; }

  // the address of the configuration this adapter's reference is bound to
  virtual const void *configStorage() const { return nullptr; }

  QString configPath() const { return _configPath; }

  void setConfigPath(const QString &path) {
    if (path == _configPath) return;
    _configPath = path;
    emit configPathChanged();
  }

signals:
  void editRequested(const QString &path, const QVariant &value);
  void previewRequested(const QString &path, const QVariant &value);
  void fieldChanged(const QString &path);

  void structureChanged();
  void configPathChanged();

private:
  QString _configPath;
  bool _refreshingAllFields = false;
};