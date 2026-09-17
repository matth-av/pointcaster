#pragma once

#include "config_adapter.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QVector>
#include <plugins/devices/device_plugin.h>

namespace pc::ui {

// one row per per-point attribute the device source declares
class AttributeListModel final : public QAbstractListModel {
  Q_OBJECT

  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
  enum class Role : int {
    Name = Qt::UserRole + 1,
    SourceType,
    Target,
    Precision,
    RangeMin,
    RangeMax
  };
  Q_ENUM(Role)

  struct AttributeEntry {
    QString name;
    QString sourceType;
    int target = 0;
    int precision = 0;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;

    bool operator==(const AttributeEntry &e) const = default;
  };

  explicit AttributeListModel(QObject *parent = nullptr)
      : QAbstractListModel(parent) {}

  void bind(ConfigAdapter *adapter, pc::devices::DevicePlugin *plugin);

  int rowCount(const QModelIndex &parent = QModelIndex{}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  // rebuilds the rows from what the device imported and what the config holds
  void refresh();

  // a target is read from one attribute at a time, so any other row holding
  // it goes back to None
  Q_INVOKABLE bool setTarget(int row, int target);
  Q_INVOKABLE bool setPrecision(int row, int precision);
  Q_INVOKABLE bool setRange(int row, float rangeMin, float rangeMax);

signals:
  void countChanged();

private:
  template <typename Edit> bool editRow(int row, Edit &&edit);

  QPointer<ConfigAdapter> _adapter;
  pc::devices::DevicePlugin *_plugin = nullptr;
  QVector<AttributeEntry> _entries;
};

} // namespace pc::ui
