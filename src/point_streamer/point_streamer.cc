#include "point_streamer.h"
#include "networking/zmq_context.h"
#include "point_streamer/point_streamer_config.h"

#include <algorithm>
#include <chrono>
#include <core/logger/logger.h>
#include <core/profiling/profiling_zone.h>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <networking/zmq_context.h>
#include <optional>
#include <point_streamer/stream_channels.h>
#include <ranges>
#include <session/session.h>
#include <span>
#include <string>
#include <thread>
#include <util/string_map.h>
#include <vector>
#include <workspace/workspace.h>
#include <workspace/workspace_config.h>
#include <zmq.hpp>
#include <zpp_bits.h>

// tbb hates qt emit macro i think...
#pragma push_macro("emit")
#undef emit
#include <oneapi/tbb/parallel_for.h>
#pragma pop_macro("emit")

using namespace std::chrono;
using namespace pc::profiling;

namespace pc::networking {

namespace {

constexpr auto period_for(int hz) {
  return duration_cast<steady_clock::duration>(
      duration<double>(1.0 / std::max(hz, 1)));
}

bool channel_enabled(
    const std::span<const StreamChannelConfiguration> channel_configs,
    const std::string_view address) {
  const auto it =
      std::find_if(channel_configs.begin(), channel_configs.end(),
                   [&](auto &config) { return config.address == address; });
  if (it == channel_configs.end()) return true;
  return it->enabled;
}

struct SocketSettings {
  std::string address;
  int port = 0;
  int send_high_water_mark = 0;

  bool operator==(const SocketSettings &) const = default;
};

std::optional<zmq::socket_t> bind_socket(const SocketSettings &settings) {
  zmq::socket_t socket{zmq_context(), zmq::socket_type::xpub};
  socket.set(zmq::sockopt::sndhwm, settings.send_high_water_mark);
  socket.set(zmq::sockopt::linger, 0);
  // adding the verboser with xpub ensures we get
  // subscribe events & "unsubscribe" events for clients that drop off,
  socket.set(zmq::sockopt::xpub_verboser, 1);

  const auto endpoint =
      std::format("tcp://{}:{}", settings.address, settings.port);
  try {
    socket.bind(endpoint);
  } catch (const zmq::error_t &e) {
    pc::logger()->error("Point streamer failed to bind {} ({})", endpoint,
                        e.what());
    return std::nullopt;
  } catch (...) {
    pc::logger()->error("Point streamer failed to bind {} (Unknown exception)",
                        endpoint);
    return std::nullopt;
  }
  pc::logger()->info("Point streamer bound to {} (max {} queued frames)",
                     endpoint, settings.send_high_water_mark);
  return socket;
}

void streaming_thread_loop(
    std::stop_token stop_token, Workspace &workspace,
    std::atomic<std::shared_ptr<const StringMap<int>>> &subscriber_counts_out) {

  std::string address;
  int port;
  int publish_hz = 30;
  CodecConfiguration codec_config;
  bool publish_every_frame = false;
  Toggleable<int> max_queued_frames;
  std::vector<PointStream> point_streams;
  std::vector<StreamChannelConfiguration> channel_configs;

  // syncing access... pattern here?
  // TODO this config sync with new local values could be made generic
  // somehow, ive used it over configs elsewhere / check mqtt_client.cc
  // ALSO the whole syncing strings and collections each frame seems wasteful
  const auto sync_config_vars = [&] {
    std::lock_guard lock(workspace.config_access);
    const auto &stream_config = workspace.config.point_streamer;
    address = stream_config.address;
    port = stream_config.port;
    publish_hz = stream_config.publish_hz;
    codec_config = stream_config.codec_config;
    publish_every_frame = stream_config.publish_every_frame;
    max_queued_frames = stream_config.max_queued_frames;
    // TODO is this too heavy to do every frame? maybe we need a dirty marker
    channel_configs = stream_config.channels;
    point_streams = collect_point_streams(workspace);
  };

  std::optional<zmq::socket_t> pub_socket;
  SocketSettings attempted_settings;
  constexpr auto connection_retry_interval = 2s;
  auto next_connection_attempt = steady_clock::now();

  // per-stream cached state, key is the streams address
  StringMap<std::shared_ptr<PointCloud>> last_clouds;
  // pre-serialized data: address + '\0' + serialized payload
  StringMap<std::shared_ptr<std::vector<std::byte>>> last_data;

  // an empty point cloud that is sent on the frame a channel transitions
  // from publishing to not publishing
  const auto stopped_cloud = std::make_shared<PointCloud>();

  StringMap<int> subscriber_counts;
  bool subscriber_counts_dirty = true;

  std::vector<std::string> newly_subscribed_addresses;

  const auto handle_subscriber_message = [&](auto &msg) {
    if (msg.size() < 1) return;
    const auto *bytes = static_cast<const std::byte *>(msg.data());
    const bool subscribe = static_cast<unsigned char>(bytes[0]) == 1;
    std::string topic(reinterpret_cast<const char *>(bytes) + 1,
                      msg.size() - 1);
    if (!topic.empty() && topic.back() == '\0') topic.pop_back();
    auto &count = subscriber_counts[topic];
    if (subscribe) {
      count++;
      newly_subscribed_addresses.push_back(topic);
    } else if (count > 0) {
      count--;
    }
    subscriber_counts_dirty = true;
  };

  const auto has_subscriber = [&](const std::string_view stream_address) {
    return std::ranges::any_of(subscriber_counts, [&](const auto &entry) {
      return entry.second > 0 && stream_address.starts_with(entry.first);
    });
  };

  // ---- processing loop ----

  auto next_tick = steady_clock::now();

  while (!stop_token.stop_requested()) {

    sync_config_vars();

    const auto enabled_channel_count =
        std::ranges::count_if(point_streams, [&](const auto &stream) {
          return channel_enabled(channel_configs, stream.address);
        });

    // 2x the number of channels is the least that always fits a whole tick
    const auto queued_frame_limit = [&]() {
      if (max_queued_frames.active) return std::max(max_queued_frames.value, 1);
      return std::max(2 * static_cast<int>(enabled_channel_count), 2);
    };

    const SocketSettings socket_settings{
        .address = address.empty() ? "*" : address,
        .port = port,
        .send_high_water_mark = queued_frame_limit()};

    const bool connection_retry_due =
        !pub_socket && steady_clock::now() >= next_connection_attempt;
    if (socket_settings != attempted_settings || connection_retry_due) {
      pub_socket.reset();
      pub_socket = bind_socket(socket_settings);
      attempted_settings = socket_settings;
      next_connection_attempt = steady_clock::now() + connection_retry_interval;
      subscriber_counts.clear();
      newly_subscribed_addresses.clear();
      subscriber_counts_dirty = true;
    }

    if (pub_socket) {
      for (zmq::message_t msg;
           pub_socket->recv(msg, zmq::recv_flags::dontwait);) {
        handle_subscriber_message(msg);
      }
    }

    std::vector<PointStream> subscribed_streams;
    std::vector<PointStream> publishing_streams;
    {
      ProfilingZone collect_streams_zone("point_stream::collect_streams");
      publishing_streams =
          point_streams | std::views::filter([&](const auto &stream) {
            return has_subscriber(stream.address) &&
                   channel_enabled(channel_configs, stream.address);
          }) |
          std::ranges::to<std::vector>();
    }

    // any channel that was publishing, but isn't publishing any more sends one
    // last empty pointcloud on its channel to clear the clients
    std::vector<PointStream> stopped_streams;
    {
      const auto still_streaming = [&](const std::string &channel_address) {
        const auto it = std::ranges::find(publishing_streams, channel_address,
                                          &PointStream::address);
        return it != publishing_streams.end() && it->cloud &&
               !it->cloud->empty();
      };
      stopped_streams = last_clouds | std::views::keys |
                        std::views::filter([&](const auto &channel_address) {
                          return !still_streaming(channel_address);
                        }) |
                        std::views::transform([&](const auto &channel_address) {
                          return PointStream{channel_address, stopped_cloud};
                        }) |
                        std::ranges::to<std::vector>();

      for (const auto &stopped : stopped_streams) {
        const auto it = std::ranges::find(publishing_streams, stopped.address,
                                          &PointStream::address);
        if (it != publishing_streams.end()) {
          it->cloud = stopped_cloud;
        } else {
          publishing_streams.push_back(stopped);
        }
      }
    }

    std::vector<PointStream> streams_to_serialize;
    {
      ProfilingZone collect_dirty_zone("point_stream::collect_dirty");
      streams_to_serialize =
          publishing_streams | std::views::filter([&](const auto &stream) {
            if (!stream.cloud) return false;
            if (stream.cloud->empty() && stream.cloud != stopped_cloud) {
              return false;
            }
            const auto it = last_clouds.find(stream.address);
            return it == last_clouds.end() || it->second != stream.cloud;
          }) |
          std::ranges::to<std::vector>();
    }

    // serialize all changed channels in parallel, then merge results
    // sequentially before any sending happens
    if (!streams_to_serialize.empty()) {

      ProfilingZone serialize_zone("point_stream::serialize");

      std::vector<std::shared_ptr<std::vector<std::byte>>> serialized_frames(
          streams_to_serialize.size());

      {
        ProfilingZone parallel_serialize_zone("parallel_serialize");
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, streams_to_serialize.size()),
            [&](const tbb::blocked_range<size_t> &range) {
              for (size_t i = range.begin(); i < range.end(); i++) {
                const auto &stream = streams_to_serialize[i];

                const auto payload = stream.cloud->compress(codec_config);

                const auto prefix_size = stream.address.size() + 1;

                auto framed = std::make_shared<std::vector<std::byte>>(
                    prefix_size + payload.size());
                std::memcpy(framed->data(), stream.address.data(),
                            stream.address.size());
                (*framed)[stream.address.size()] = std::byte{0};
                std::memcpy(framed->data() + prefix_size, payload.data(),
                            payload.size());

                serialized_frames[i] = std::move(framed);
              }
            });
      }
      {
        ProfilingZone cache_frames_zone("cache_frames");
        for (size_t i = 0; i < streams_to_serialize.size(); i++) {
          const auto &stream = streams_to_serialize[i];
          last_clouds[stream.address] = stream.cloud;
          last_data[stream.address] = std::move(serialized_frames[i]);
        }
      }
    }

    // if publish_every_frame is true, every publishing channel re-sends its
    // last frame. otherwise a channel only sends when its cloud changed, or
    // when a client just subscribed and needs the last frame it missed
    if (pub_socket) {
      ProfilingZone send_zone("point_stream::send");
      for (const auto &stream : publishing_streams) {

        if (!publish_every_frame) {
          const auto frame_changed =
              std::ranges::find(streams_to_serialize, stream.address,
                                &PointStream::address) !=
              streams_to_serialize.end();
          const auto has_new_subscriber = std::ranges::any_of(
              newly_subscribed_addresses, [&](const auto &topic) {
                return stream.address.starts_with(topic);
              });
          if (!frame_changed && !has_new_subscriber) continue;
        }

        const auto it = last_data.find(stream.address);
        if (it == last_data.end() || !it->second || it->second->empty())
          continue;

        // hand libzmq the bytes (no copy), keep them alive via a heap
        // shared_ptr freed by the deleter once the send completes
        auto *hint = new std::shared_ptr<std::vector<std::byte>>(it->second);
        zmq::message_t msg(
            it->second->data(), it->second->size(),
            [](void *, void *h) {
              delete static_cast<std::shared_ptr<std::vector<std::byte>> *>(h);
            },
            hint);
        try {
          pub_socket->send(msg, zmq::send_flags::none);
        } catch (const zmq::error_t &e) {
          pc::logger()->warn("Point streamer send failed on '{}': {}",
                             stream.address, e.what());
        }
      }
    }

    newly_subscribed_addresses.clear();

    // remove stopped streams from our cache
    for (const auto &stream : stopped_streams) {
      last_clouds.erase(stream.address);
      last_data.erase(stream.address);
    }

    if (subscriber_counts_dirty) {
      subscriber_counts_out.store(
          std::make_shared<const StringMap<int>>(subscriber_counts),
          std::memory_order_release);
      subscriber_counts_dirty = false;
    }

    next_tick =
        std::max(next_tick + period_for(publish_hz), steady_clock::now());
    std::this_thread::sleep_until(next_tick);
  }
}
} // namespace

PointStreamer::PointStreamer(Workspace &workspace)
    : _worker(streaming_thread_loop, std::ref(workspace),
              std::ref(_subscriber_counts)) {}

bool PointStreamer::has_listeners(const std::string &channel_address) const {
  const auto counts = _subscriber_counts.load(std::memory_order_acquire);
  if (!counts) return false;
  const auto subscribe_all = counts->find("");
  if (subscribe_all != counts->end() && subscribe_all->second > 0) return true;
  const auto it = counts->find(channel_address);
  return it != counts->end() && it->second > 0;
}

std::shared_ptr<const StringMap<int>> PointStreamer::subscriber_counts() const {
  return _subscriber_counts.load(std::memory_order_acquire);
}

} // namespace pc::networking
