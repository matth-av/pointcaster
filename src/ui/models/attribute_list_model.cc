#include "attribute_list_model.h"

#include <QVariant>
#include <algorithm>
#include <config/attribute_config.h>
#include <variant>

namespace pc::ui {

namespace {

constexpr auto attributes_path = "attributes";

} // namespace

void AttributeListModel::bind(ConfigAdapter *adapter,
                              pc::devices::DevicePlugin *plugin) {
  _adapter = adapter;
  _plugin = plugin;
  refresh();
}

int AttributeListModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(_entries.size());
}

QHash<int, QByteArray> AttributeListModel::roleNames() const {
  return {{static_cast<int>(Role::Name), "name"},
          {static_cast<int>(Role::SourceType), "sourceType"},
          {static_cast<int>(Role::Target), "target"},
          {static_cast<int>(Role::Precision), "precision"},
          {static_cast<int>(Role::RangeMin), "rangeMin"},
          {static_cast<int>(Role::RangeMax), "rangeMax"}};
}

QVariant AttributeListModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= _entries.size()) {
    return {};
  }
  const auto &entry = _entries[index.row()];
  switch (static_cast<Role>(role)) {
  case Role::Name:
    return entry.name;
  case Role::SourceType:
    return entry.sourceType;
  case Role::Target:
    return entry.target;
  case Role::Precision:
    return entry.precision;
  case Role::RangeMin:
    return entry.rangeMin;
  case Role::RangeMax:
    return entry.rangeMax;
  }
  return {};
}

void AttributeListModel::refresh() {
  QVector<AttributeEntry> entries;

  if (_plugin) {
    const auto imported = _plugin->imported_attributes();

    std::visit(
        [&](auto &config) {
          // if the config contains an attributes member,
          // for instance the PlyDeviceConfiguration
          if constexpr (requires { config.attributes; }) {
            const auto &rows = config.attributes;
            for (const auto &attribute : imported.attributes) {
              const auto stored =
                  std::ranges::find_if(rows, [&](const auto &candidate) {
                    return candidate.name == attribute.name;
                  });
              if (stored == rows.end()) continue;

              entries.push_back(
                  {.name = QString::fromStdString(stored->name),
                   .sourceType = QString::fromStdString(attribute.type_name),
                   .target = static_cast<int>(stored->target),
                   .precision = static_cast<int>(stored->precision),
                   .rangeMin = stored->range_min,
                   .rangeMax = stored->range_max});
            }
          }
        },
        _plugin->config());
  }

  if (entries == _entries) return;

  const bool countChanges = entries.size() != _entries.size();
  if (countChanges) beginResetModel();
  _entries = std::move(entries);
  if (countChanges) {
    endResetModel();
    emit countChanged();
    return;
  }
  if (_entries.isEmpty()) return;
  emit dataChanged(index(0), index(rowCount() - 1));
}

// edit is handed every row along with the one being edited, so a change can
// reach past its own row
template <typename Edit>
bool AttributeListModel::editRow(int row, Edit &&edit) {
  if (!_adapter || !_plugin) return false;
  if (row < 0 || row >= _entries.size()) return false;
  const auto name = _entries[row].name.toStdString();

  bool changed = false;
  std::visit(
      [&](auto &config) {
        if constexpr (requires { config.attributes; }) {
          auto updated = config.attributes;
          const auto stored =
              std::ranges::find_if(updated, [&](const auto &candidate) {
                return candidate.name == name;
              });
          if (stored == updated.end()) return;

          edit(updated, *stored);
          if (updated == config.attributes) return;

          _adapter->set(QString::fromLatin1(attributes_path),
                        QVariant::fromValue(updated));
          changed = true;
        }
      },
      _plugin->config());

  if (changed) refresh();
  return changed;
}

bool AttributeListModel::setTarget(int row, int target) {
  return editRow(row, [&](auto &rows, auto &stored) {
    if (target < 0 || target > static_cast<int>(AttributeTarget::PointScale)) {
      return;
    }
    const auto chosen = static_cast<AttributeTarget>(target);
    if (chosen != AttributeTarget::None) {
      for (auto &other : rows) {
        if (other.target == chosen) {
          other.target = AttributeTarget::None;
        }
      }
    }
    stored.target = chosen;
  });
}

bool AttributeListModel::setPrecision(int row, int precision) {
  return editRow(row, [&](auto &, auto &stored) {
    if (precision < 0 ||
        precision > static_cast<int>(AttributePrecision::Bits8)) {
      return;
    }
    stored.precision = static_cast<AttributePrecision>(precision);
  });
}

bool AttributeListModel::setRange(int row, float rangeMin, float rangeMax) {
  return editRow(row, [&](auto &, auto &stored) {
    stored.range_min = rangeMin;
    stored.range_max = rangeMax;
  });
}

} // namespace pc::ui
