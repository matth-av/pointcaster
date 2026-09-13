#include "message_streamer.h"

#include <chrono>
#include <logger/logger.h>
#include <mutex>
#include <networking/zmq_context.h>
#include <readerwriterqueue/readerwriterqueue.h>
#include <workspace/workspace.h>
#include <zmq.hpp>
#include <zpp_bits.h>

namespace pc::publishers {

namespace {

// keep a cache of the last published values for each path so that we can retain
// messages and re-send them when clients subscribe to new paths
StringMap<ConfigValue> last_value_for_path;

// to hold any paths that need to be force-published on the sending thread
moodycamel::ReaderWriterQueue<std::string> republish_paths;

std::unique_ptr<zmq::socket_t>
create_socket(const MessageStreamerConfiguration &config) {
  if (!config.enabled.value()) {
    return nullptr;
  }
  const auto &interface_ip = config.interface_ip.value();
  const auto port = config.port.value();
  const auto address = std::format("tcp://{}:{}", interface_ip, port);

  std::unique_ptr<zmq::socket_t> socket_ptr;
  try {
    // xpub allows us to track subscription & unsubscriptions from clients
    socket_ptr = std::make_unique<zmq::socket_t>(pc::networking::zmq_context(),
                                                 zmq::socket_type::xpub);
    socket_ptr->set(zmq::sockopt::xpub_verboser, 1);
    // TODO what is appropriate for sndhwm?
    socket_ptr->set(zmq::sockopt::sndhwm, 32);
    socket_ptr->set(zmq::sockopt::linger, 0);
    socket_ptr->bind(address);
  } catch (const zmq::error_t &e) {
    pc::logger()->error("Message streamer failed to bind to {} ({})", address,
                        e.what());
    return nullptr;
  } catch (...) {
    pc::logger()->error(
        "Message streamer failed to bind to {} (Unknown exception)", address);
    return nullptr;
  }
  pc::logger()->info("Message streamer bound at {}", address);
  return socket_ptr;
}

void handle_subscriber_message(zmq::message_t &msg,
                               MessageStreamer::Subscribers &subscribers) {
  if (msg.size() < 1) return;
  const auto *bytes = static_cast<const std::byte *>(msg.data());
  const bool subscribe = static_cast<unsigned char>(bytes[0]) == 1;
  std::string topic(reinterpret_cast<const char *>(bytes) + 1, msg.size() - 1);
  if (!topic.empty() && topic.back() == '\0') topic.pop_back();
  auto &count = subscribers.count_for_path[topic];
  if (subscribe) {
    count++;
    pc::logger()->trace("Gained message subscriber for '{}'", topic);
    // force a re-publish of this path so the new subscriber receives a retained
    // value to start with. Even though we received a subscribe message,
    // zmq doesn't actually garuntee the subscriber's socket is open and
    // receiving at this point, so we need to wait a bit and then
    // force the re-publish... we can do that waiting on another thread
    std::thread([topic = std::move(topic)] {
      // 200ms is meant to be enough time for slow joiners according to zmq docs
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      auto ret = republish_paths.enqueue(std::move(topic));
      pc::logger()->debug("{}", ret ? "true" : "false");
    }).detach();
  } else if (count > 0) {
    count--;
    pc::logger()->trace("Dropped message subscriber for '{}'", topic);
  }
  subscribers.dirty = true;
}

} // namespace

MessageStreamer::MessageStreamer(Workspace &workspace)
    : _listener(*this, workspace, workspace.config_registry) {
  _socket = create_socket(config(workspace));
}

void MessageStreamer::tick(
    const MessageStreamerConfiguration &config_snapshot) {
  if (!config_snapshot.enabled.value()) return;

  // if we have no connected socket, we can attempt reconnections on each tick
  if (!_socket || _socket->handle() == nullptr) {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    constexpr auto connection_retry_interval = 2s;
    thread_local static steady_clock::time_point next_connection_attempt =
        steady_clock::now() + connection_retry_interval;

    if (steady_clock::now() < next_connection_attempt) return;
    _socket.reset();
    _socket = create_socket(config_snapshot);
    next_connection_attempt = steady_clock::now() + connection_retry_interval;
  }
  // if we do have a connected and running socket, we can check our socket
  // for any incoming sub/unsub messages to update our client list
  else if (_socket->handle() != nullptr) {
    for (zmq::message_t msg; _socket->recv(msg, zmq::recv_flags::dontwait);) {
      handle_subscriber_message(msg, subscribers);
    }
    // and if we have any paths that need to be republished to new subscribers
    // after potentially updating our client list....
    thread_local std::string path;
    while (republish_paths.try_dequeue(path)) {
      const auto last_value = last_value_for_path[path];
      handle_update(path, last_value, config_snapshot);
    }
  }
}

void MessageStreamer::handle_config_change(
    std::string_view path,
    const MessageStreamerConfiguration &config_snapshot) {
  if (path.ends_with("enabled") || path.ends_with("port") ||
      path.ends_with("interface_ip")) {
    _socket.reset();
    _socket = create_socket(config_snapshot);
  }
}

MessageStreamerConfiguration
MessageStreamer::config(Workspace &workspace) const {
  std::scoped_lock lock(workspace.config_access);
  return workspace.config.publishers.value().message_streamer.value();
}

void MessageStreamer::handle_update(const std::string_view path,
                                    const ConfigValue &value,
                                    const MessageStreamerConfiguration &) {
  last_value_for_path[path] = value;

  if (!_socket) return;

  // if there are no subscribers, don't bother serializing and publishing
  if (subscribers.count_for_path[""] == 0 &&
      subscribers.count_for_path[path] == 0) {
    return;
  }

  // so we don't need to re-allocate memory every send:
  thread_local std::vector<std::byte> payload;
  thread_local std::string topic;

  zpp::bits::out serialize{payload};
  const auto result = serialize(value);
  if (zpp::bits::failure(result)) {
    pc::logger()->error("Failure to serialize '{}'", path);
    return;
  }

  // ensure the path/topic has a trailing null terminator char, otherwise a
  // subscription to e.g. "device/scale" also matches "device/scale_mode"
  topic.assign(path);
  if (!topic.ends_with('\0')) topic.push_back('\0');

  try {
    _socket->send(zmq::buffer(topic), zmq::send_flags::sndmore);
    _socket->send(zmq::buffer(payload.data(), serialize.position()),
                  zmq::send_flags::none);
  } catch (const zmq::error_t &e) {
    pc::logger()->error("Failure to publish '{}' ({})", path, e.what());
  }
}

} // namespace pc::publishers