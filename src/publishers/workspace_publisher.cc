#include "workspace_publisher.h"

#include <algorithm>
#include <chrono>
#include <config/config_registry.h>
#include <networking/zmq_context.h>
#include <thread>
#include <util/string_map.h>
#include <workspace/workspace.h>
#include <zmq.hpp>
#include <zpp_bits.h>

namespace pc::publishers {

namespace {

using namespace std::chrono;

constexpr auto period_for(int hz) {
  return duration_cast<steady_clock::duration>(
      duration<double>(1.0 / std::max(hz, 1)));
}

bool has_payload(const ConfigValue &value) {
  // regular value types always hold payloads...
  // for a point cloud stream, check the ptr the config value holds is valid
  return std::visit(
      [](const auto &held_value) {
        using ValueType = std::decay_t<decltype(held_value)>;
        if constexpr (is_cloud_stream_v<ValueType>) {
          return held_value != nullptr;
        } else {
          return true;
        }
      },
      value);
}

void publisher_thread_loop(std::stop_token stop_token, Workspace &workspace) {

  auto &ctx = pc::networking::zmq_context();
  zmq::socket_t pub_socket{ctx, zmq::socket_type::pub};
  try {
    pub_socket.bind("inproc://workspace");
  } catch (const zmq::error_t &e) {
    pc::logger()->error(
        "Workspace publisher failed to bind socket 'inproc://workspace'");
    return;
  }

  auto next_tick = steady_clock::now();

  StringMap<ConfigValue> current_snapshot;
  StringMap<ConfigValue> previous_snapshot;

  std::set<std::string> publish_paths;
  std::set<std::string> push_paths;
  int publish_hz;

  while (!stop_token.stop_requested()) {

    {
      std::scoped_lock lock(workspace.config_access);
      publish_paths = workspace.config.publish_paths;
      push_paths = workspace.config.push_paths;
      publish_hz = std::max(1, workspace.config.publishers.publish_hz);
    }

    current_snapshot.swap(previous_snapshot);
    workspace.config_registry.snapshot(publish_paths, current_snapshot);

    for (const auto &kvp : current_snapshot) {
      const auto &path = kvp.first;
      const auto &value = kvp.second;
      if (!has_payload(value)) continue;

      const auto previous = previous_snapshot.find(kvp.first);
      const bool changed =
          previous == previous_snapshot.end() || previous->second != value;

      // pushed paths go out every tick, published paths only when they change.
      // a stream compares by pointer, so a cloud produced this tick is always
      // a change and the same cloud twice never is
      if (changed || push_paths.contains(path)) {
        // serialize our variant into bytes
        auto [data, serialize] = zpp::bits::data_out();
        const auto result = serialize(kvp);
        if (zpp::bits::failure(result)) {
          pc::logger()->error("Failure to serialize '{}'", path);
          continue;
        }
        zmq::message_t msg(std::move(data));
        try {
          pub_socket.send(msg, zmq::send_flags::none);
        } catch (const zmq::error_t &e) {
          pc::logger()->error("Failure to publish '{}' ({})", path, e.what());
          continue;
        }
      }
    }

    const int hz = std::max(publish_hz, 1);
    next_tick = std::max(next_tick + period_for(hz), steady_clock::now());
    std::this_thread::sleep_until(next_tick);
  }
}

} // namespace

WorkspacePublisher::WorkspacePublisher(Workspace &workspace)
    : _worker(publisher_thread_loop, std::ref(workspace)) {}

} // namespace pc::publishers
