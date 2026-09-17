#include "stream_channel_model.h"
#include "point_streamer/point_streamer_config.h"

#include <QTimer>
#include <algorithm>
#include <mutex>
#include <point_streamer/point_streamer.h>
#include <point_streamer/stream_channels.h>
#include <unordered_map>
#include <workspace/workspace.h>

namespace pc::ui {

StreamChannelListModel::StreamChannelListModel(pc::Workspace *workspace,
                                               QObject *parent)
    : QAbstractListModel(parent), _workspace(workspace) {
  // poll to keep the status of each stream realtime
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this,
          &StreamChannelListModel::pollChannelStatuses);
  timer->start(250);
}

int StreamChannelListModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid()) return 0;
  return static_cast<int>(_channels.size());
}

QVariant StreamChannelListModel::data(const QModelIndex &index,
                                      int role) const {
  if (!index.isValid()) return {};
  const int row = index.row();
  if (row < 0 || row >= rowCount()) return {};

  const ChannelEntry &e = _channels[row];
  switch (static_cast<Role>(role)) {
  case Role::Address:
    return e.address;
  case Role::Enabled:
    return e.enabled;
  case Role::Status:
    return static_cast<int>(channelStatus(row));
  }
  return {};
}

QHash<int, QByteArray> StreamChannelListModel::roleNames() const {
  return {
      {static_cast<int>(Role::Address), "address"},
      {static_cast<int>(Role::Enabled), "enabled"},
      {static_cast<int>(Role::Status), "status"},
  };
}

void StreamChannelListModel::refresh() {
  QVector<ChannelEntry> channels;

  if (_workspace) {
    std::lock_guard lock(_workspace->config_access);
    const auto &stream_config = _workspace->config.point_streamer;

    std::unordered_map<std::string, bool> enabled_overrides;
    for (const auto &channel : stream_config.channels)
      enabled_overrides[channel.address] = channel.enabled;

    for (const auto &source :
         pc::networking::collect_point_streams(*_workspace)) {
      bool enabled = true;
      if (const auto it = enabled_overrides.find(source.address);
          it != enabled_overrides.end())
        enabled = it->second;
      channels.push_back({QString::fromStdString(source.address), enabled});
    }
  }

  beginResetModel();
  _channels = std::move(channels);
  for (int i = 0; i < _channels.size(); ++i)
    _channels[i].status = channelStatus(i);
  endResetModel();
}

bool StreamChannelListModel::setChannelEnabled(int index, bool enabled) {
  if (index < 0 || index >= rowCount()) return false;
  if (_channels[index].enabled == enabled) return false;
  _channels[index].enabled = enabled;
  _channels[index].status = channelStatus(index);

  if (_workspace) {
    const std::string address = _channels[index].address.toStdString();
    std::lock_guard lock(_workspace->config_access);
    auto &channels = _workspace->config.point_streamer.channels;
    auto it = std::find_if(channels.begin(), channels.end(),
                           [&](const auto &c) { return c.address == address; });
    if (it != channels.end()) {
      it->enabled = enabled;
    } else {
      channels.push_back(pc::networking::StreamChannelConfiguration{
          .address = address,
          .enabled = {enabled},
      });
    }
  }

  const auto modelIndex = this->index(index);
  emit dataChanged(
      modelIndex, modelIndex,
      {static_cast<int>(Role::Enabled), static_cast<int>(Role::Status)});
  return true;
}

QString StreamChannelListModel::channelAddress(int index) const {
  if (index < 0 || index >= rowCount()) return {};
  return _channels[index].address;
}

StreamChannelStatus StreamChannelListModel::channelStatus(int index) const {
  if (index < 0 || index >= rowCount()) return StreamChannelStatus::Disabled;
  const ChannelEntry &e = _channels[index];
  if (!e.enabled) return StreamChannelStatus::Disabled;
  if (_workspace && _workspace->point_streamer &&
      _workspace->point_streamer->has_listeners(e.address.toStdString()))
    return StreamChannelStatus::Connected;
  return StreamChannelStatus::Live;
}

void StreamChannelListModel::pollChannelStatuses() {
  for (int i = 0; i < _channels.size(); ++i) {
    const auto status = channelStatus(i);
    if (_channels[i].status == status) continue;
    _channels[i].status = status;
    const auto modelIndex = index(i);
    emit dataChanged(modelIndex, modelIndex, {static_cast<int>(Role::Status)});
  }
}

} // namespace pc::ui
