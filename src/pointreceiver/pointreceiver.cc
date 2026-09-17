#include "pointreceiver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <config/config_value.h>
#include <core/logger/logger.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pointcaster/core_types.h>
#include <pointcaster/point_cloud.h>
#include <ranges>
#include <readerwriterqueue/readerwriterqueue.h>
#include <set>
#include <span>
#include <spdlog/common.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <util/string_map.h>
#include <variant>
#include <vector>
#include <zmq.hpp>
#include <zpp_bits.h>

namespace {

// to make sure exceptions dont cross into the api, exceptional code is
// wrapped with this template:
template <class Operation>
pointreceiver_status exception_boundary(std::string_view operation_name,
                                        Operation &&operation) noexcept {
  try {
    return operation();
  } catch (const std::bad_alloc &) {
    pc::logger()->error("{}: out of memory", operation_name);
    return POINTRECEIVER_ERROR_OUT_OF_MEMORY;
  } catch (const std::exception &error) {
    pc::logger()->error("{}: {}", operation_name, error.what());
    return POINTRECEIVER_ERROR_INTERNAL;
  } catch (...) {
    pc::logger()->error("{}: unknown exception", operation_name);
    return POINTRECEIVER_ERROR_INTERNAL;
  }
}

} // namespace

namespace pc::receiver {

using namespace std::chrono;
using namespace std::chrono_literals;

using SubscriptionSet = std::unordered_set<std::string>;
using SubscriptionSnapshot = std::shared_ptr<const SubscriptionSet>;

struct PointCloudFrame {
  std::uint64_t timestamp = 0;
  bool pending = false;
  // the newest cloud coming from the receiver thread
  std::shared_ptr<const PointCloud> incoming_cloud;
  // the cloud being used by the caller that dequeues
  std::shared_ptr<const PointCloud> current_cloud;
  std::vector<pointreceiver_attribute> current_attributes;
};

using MessageFrame = std::pair<std::string, ConfigValue>;

namespace {

// every receiver context in this process shares one zmq context
std::shared_ptr<zmq::context_t> acquire_zmq_ctx() {
  // TODO what's an appropriate zmq io thread count to use?
  constexpr auto zmq_io_thread_count = 1;
  static std::mutex zmq_ctx_mutex;
  static std::weak_ptr<zmq::context_t> zmq_ctx;

  std::lock_guard lock(zmq_ctx_mutex);
  if (auto ctx = zmq_ctx.lock()) return ctx;
  auto ctx = std::make_shared<zmq::context_t>(zmq_io_thread_count);
  zmq_ctx = ctx;
  return ctx;
}

} // namespace

struct Context {
  std::shared_ptr<zmq::context_t> zmq_ctx = acquire_zmq_ctx();

  std::optional<std::string> client_name;

  // TODO should this be bounded?
  moodycamel::BlockingReaderWriterQueue<MessageFrame> message_queue;

  std::mutex message_subscription_mutex;
  SubscriptionSnapshot message_subscriptions =
      std::make_shared<const SubscriptionSet>();
  std::set<std::string> known_message_addresses;

  std::mutex point_cloud_subscription_mutex;
  SubscriptionSnapshot point_cloud_subscriptions =
      std::make_shared<const SubscriptionSet>();
  std::set<std::string> known_point_cloud_addresses;

  std::mutex point_cloud_stream_mutex;
  std::condition_variable point_cloud_stream_cv;
  pc::StringMap<pc::receiver::PointCloudFrame> point_cloud_frames;
  std::uint64_t point_cloud_stream_timestamp = 0;

  std::atomic<bool> message_stopping{false};
  std::thread message_worker;

  std::atomic<bool> point_cloud_stopping{false};
  std::thread point_cloud_worker;

  void stop_message_worker() {
    message_stopping.store(true, std::memory_order_release);
    if (message_worker.joinable()) message_worker.join();
  }

  void stop_point_cloud_worker() {
    point_cloud_stopping.store(true, std::memory_order_release);
    if (point_cloud_worker.joinable()) point_cloud_worker.join();
  }

  ~Context() {
    stop_point_cloud_worker();
    stop_message_worker();
  }
};

namespace {

static_assert(sizeof(pointreceiver_status) == sizeof(int));
static_assert(sizeof(pointreceiver_position_t) == sizeof(pc::position));
static_assert(alignof(pointreceiver_position_t) == alignof(pc::position));
static_assert(offsetof(pointreceiver_position_t, x) ==
              offsetof(pc::position, x));
static_assert(offsetof(pointreceiver_position_t, y) ==
              offsetof(pc::position, y));
static_assert(offsetof(pointreceiver_position_t, z) ==
              offsetof(pc::position, z));
static_assert(sizeof(pointreceiver_color_t) == sizeof(pc::color));
static_assert(alignof(pointreceiver_color_t) == alignof(pc::color));
static_assert(offsetof(pointreceiver_color_t, r) == offsetof(pc::color, r));
static_assert(offsetof(pointreceiver_color_t, a) == offsetof(pc::color, a));

// the scalar widths the api can name, kept in step with
// pointreceiver_attribute_type
template <typename Scalar> constexpr pointreceiver_attribute_type wire_type() {
  if constexpr (std::is_same_v<Scalar, float>) {
    return POINTRECEIVER_ATTRIBUTE_FLOAT32;
  } else if constexpr (std::is_same_v<Scalar, std::uint8_t>) {
    return POINTRECEIVER_ATTRIBUTE_UINT8;
  } else if constexpr (std::is_same_v<Scalar, std::int8_t>) {
    return POINTRECEIVER_ATTRIBUTE_INT8;
  } else if constexpr (std::is_same_v<Scalar, std::uint16_t>) {
    return POINTRECEIVER_ATTRIBUTE_UINT16;
  } else if constexpr (std::is_same_v<Scalar, std::uint32_t>) {
    return POINTRECEIVER_ATTRIBUTE_UINT32;
  } else if constexpr (std::is_same_v<Scalar, std::int16_t>) {
    return POINTRECEIVER_ATTRIBUTE_INT16;
  } else if constexpr (std::is_same_v<Scalar, std::int32_t>) {
    return POINTRECEIVER_ATTRIBUTE_INT32;
  } else {
    static_assert(false, "attribute scalar has no wire type");
  }
}

bool copy_to_buffer(char *destination, size_t capacity,
                    std::string_view source) {
  const auto length = source.copy(destination, capacity - 1);
  destination[length] = '\0';
  return length == source.size();
}

// load the latest subscriptions using load_subscriptions_function,
// and ensure socket has its subscriptions synchronised with them.
// returns the newly loaded list of subscriptions.
SubscriptionSnapshot
sync_subscriptions(const SubscriptionSnapshot &current_subscriptions,
                   zmq::socket_t &socket, auto &&load_subscriptions_function) {
  const auto latest_subscriptions = load_subscriptions_function();
  if (latest_subscriptions == current_subscriptions) {
    return current_subscriptions;
  }
  // subscribe the socket to newly added subscriptions
  for (const auto &address : *latest_subscriptions) {
    if (!current_subscriptions || !current_subscriptions->contains(address)) {
      socket.set(zmq::sockopt::subscribe,
                 address.empty() ? address : address + '\0');
    }
  }
  if (!current_subscriptions || current_subscriptions->empty()) {
    return latest_subscriptions;
  }
  // unsubscribe the socket from subscriptions not present in our latest list
  for (const auto &address : *current_subscriptions)
    if (!latest_subscriptions->contains(address)) {
      socket.set(zmq::sockopt::unsubscribe,
                 address.empty() ? address : address + '\0');
    }
  return latest_subscriptions;
}

void message_receive_loop(pc::receiver::Context &ctx, zmq::socket_t socket) {
  pc::logger()->trace("Beginning message receive thread");

  // we keep a thread-local snapshot of our subscriptions collection
  // and sync it with the socket each loop
  SubscriptionSnapshot subscriptions;

  while (!ctx.message_stopping.load(std::memory_order_acquire)) {

    subscriptions = sync_subscriptions(subscriptions, socket, [&ctx] {
      std::lock_guard lock(ctx.message_subscription_mutex);
      return ctx.message_subscriptions;
    });

    // we receive multi-part messages, where the first is the message's
    // path/address, and the next is the value.
    // so the

    auto receive_flags = zmq::recv_flags::none;
    std::string current_topic;

    for (zmq::message_t message; socket.recv(message, receive_flags);) {
      receive_flags = zmq::recv_flags::dontwait;

      // if this is the first part of the multipart message,
      // store it as the incoming message's topic and move on to the next part
      if (message.more()) {
        current_topic.assign(message.to_string_view());
        continue;
      }
      // grab the value
      const std::span buffer(static_cast<const std::byte *>(message.data()),
                             message.size());
      zpp::bits::in deserialize(buffer);
      pc::ConfigValue value;
      const auto result = deserialize(value);
      if (zpp::bits::failure(result)) {
        pc::logger()->warn(
            "Failed to deserialise incoming message from Pointcaster");
        continue;
      }
      // dump into message_queue where the MessageFrame to emplace is a pair of
      // the target path string, and the value variant
      ctx.message_queue.emplace(std::move(current_topic), value);
    }
  }

  pc::logger()->trace("Message receive thread stopping");
}

void point_cloud_receive_loop(pc::receiver::Context &ctx,
                              zmq::socket_t socket) {
  pc::logger()->trace("Beginning point cloud receive thread");

  // we keep a thread-local snapshot of our subscriptions collection
  // and sync it with the socket each loop
  SubscriptionSnapshot subscriptions;

  pc::StringMap<zmq::message_t> latest_payloads;

  while (!ctx.point_cloud_stopping.load(std::memory_order_acquire)) {

    subscriptions = sync_subscriptions(subscriptions, socket, [&ctx] {
      std::lock_guard lock(ctx.point_cloud_subscription_mutex);
      return ctx.point_cloud_subscriptions;
    });

    latest_payloads.clear();

    // block for the first frame so an idle socket doesn't spin, then drain the
    // rest without blocking
    auto receive_flags = zmq::recv_flags::none;
    for (zmq::message_t message; socket.recv(message, receive_flags);) {
      receive_flags = zmq::recv_flags::dontwait;
      const auto frame = message.to_string_view();
      const auto separator = frame.find('\0');
      if (separator == std::string_view::npos) continue;
      const auto address = frame.substr(0, separator);
      latest_payloads[address] = std::move(message);
    }

    const auto message_bytes = [](const zmq::message_t &message) {
      return std::span<const std::byte>{
          static_cast<const std::byte *>(message.data()), message.size()};
    };

    for (auto &[address, payload] : latest_payloads) {
      const auto cloud_bytes =
          message_bytes(payload).subspan(address.size() + 1);

      std::shared_ptr<const PointCloud> cloud;
      try {
        cloud = std::make_shared<const PointCloud>(
            PointCloud::decompress(cloud_bytes));
      } catch (const std::exception &e) {
        pc::logger()->warn("point_cloud deserialize threw: {} (size={})",
                           e.what(), cloud_bytes.size());
        continue;
      } catch (...) {
        pc::logger()->warn(
            "point_cloud deserialize threw unknown exception (size={})",
            cloud_bytes.size());
        continue;
      }

      {
        std::lock_guard lock(ctx.point_cloud_stream_mutex);
        auto it = ctx.point_cloud_frames.find(address);
        if (it == ctx.point_cloud_frames.end()) {
          it = ctx.point_cloud_frames.emplace(address, PointCloudFrame{}).first;
          ctx.known_point_cloud_addresses.emplace(address);
        }
        it->second.incoming_cloud = std::move(cloud);
        it->second.timestamp = ++ctx.point_cloud_stream_timestamp;
        it->second.pending = true;
      }
      ctx.point_cloud_stream_cv.notify_one();
    }
  }

  pc::logger()->trace("Point cloud receive thread stopping");
}

} // namespace

} // namespace pc::receiver

// to satisfy the C-api's global scope
struct pointreceiver_context : pc::receiver::Context {};

extern "C" {

const char *pointreceiver_status_string(pointreceiver_status status) {
  switch (status) {
  case POINTRECEIVER_OK:
    return "ok";
  case POINTRECEIVER_ERROR_INVALID_ARGUMENT:
    return "invalid argument";
  case POINTRECEIVER_ERROR_ALREADY_RUNNING:
    return "already running";
  case POINTRECEIVER_ERROR_NOT_RUNNING:
    return "not running";
  case POINTRECEIVER_ERROR_CONNECTION_FAILED:
    return "connection failed";
  case POINTRECEIVER_ERROR_TIMEOUT:
    return "timed out";
  case POINTRECEIVER_ERROR_DECODE_FAILED:
    return "decode failed";
  case POINTRECEIVER_ERROR_OUT_OF_RANGE:
    return "index out of range";
  case POINTRECEIVER_ERROR_OUT_OF_MEMORY:
    return "out of memory";
  case POINTRECEIVER_ERROR_INTERNAL:
    return "internal error";
  }
  return "unknown status";
}

pointreceiver_context *pointreceiver_create_context() {
  try {
    return new pointreceiver_context();
  } catch (const std::exception &error) {
    pc::logger()->error("pointreceiver_create_context: {}", error.what());
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

void pointreceiver_destroy_context(pointreceiver_context *ctx) {
  if (!ctx) return;
  exception_boundary("pointreceiver_destroy_context", [&] {
    pointreceiver_stop_point_receiver(ctx);
    pointreceiver_stop_message_receiver(ctx);
    delete ctx;
    pc::logger()->trace("Pointreceiver context is destroyed");
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status pointreceiver_set_client_name(pointreceiver_context *ctx,
                                                   const char *client_name) {
  if (!ctx || !client_name) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_set_client_name", [&] {
    ctx->client_name = std::string(client_name);
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status
pointreceiver_start_message_receiver(pointreceiver_context *ctx,
                                     const char *pointcaster_message_address) {
  if (!ctx || !pointcaster_message_address)
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  if (ctx->message_worker.joinable()) {
    return POINTRECEIVER_ERROR_ALREADY_RUNNING;
  }

  return exception_boundary("pointreceiver_start_message_receiver", [&] {
    const std::string endpoint(pointcaster_message_address);
    zmq::socket_t socket(*ctx->zmq_ctx, zmq::socket_type::sub);

    try {
      // TODO why 32 msgs? what is an actually valid number here for the high
      // watermark?
      socket.set(zmq::sockopt::rcvhwm, 32);
      socket.set(zmq::sockopt::linger, 0);
      constexpr auto receive_thread_block_timeout_ms = 100;
      socket.set(zmq::sockopt::rcvtimeo, receive_thread_block_timeout_ms);
      socket.connect(endpoint);
    } catch (const zmq::error_t &e) {
      pc::logger()->error("Message receiver failed to connect to '{}' - {}",
                          endpoint, e.what());
      return POINTRECEIVER_ERROR_CONNECTION_FAILED;
    }

    pc::logger()->info("Messaging socket open at {}", endpoint);

    ctx->message_stopping.store(false, std::memory_order_release);
    ctx->message_worker =
        std::thread([ctx, socket = std::move(socket)]() mutable {
          exception_boundary("Message receive thread", [&] {
            pc::receiver::message_receive_loop(*ctx, std::move(socket));
            return POINTRECEIVER_OK;
          });
        });
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status
pointreceiver_stop_message_receiver(pointreceiver_context *ctx) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  if (!ctx->message_worker.joinable()) return POINTRECEIVER_ERROR_NOT_RUNNING;

  return exception_boundary("pointreceiver_stop_message_receiver", [&] {
    pc::logger()->trace("Stopping message receiver");
    ctx->stop_message_worker();
    pc::logger()->info("Message receiver thread ended");
    return POINTRECEIVER_OK;
  });
}

bool pointreceiver_message_receiver_running(pointreceiver_context *ctx) {
  return ctx && ctx->message_worker.joinable();
}

pointreceiver_status
pointreceiver_subscribe_to_message(pointreceiver_context *ctx,
                                   const char *address) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_subscribe_to_message", [&] {
    std::lock_guard lock(ctx->message_subscription_mutex);
    auto updated = std::make_shared<pc::receiver::SubscriptionSet>(
        *ctx->message_subscriptions);
    updated->insert(address ? address : "");
    ctx->message_subscriptions = std::move(updated);
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status
pointreceiver_unsubscribe_from_message(pointreceiver_context *ctx,
                                       const char *address) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_unsubscribe_from_message", [&] {
    std::lock_guard lock(ctx->message_subscription_mutex);
    auto updated = std::make_shared<pc::receiver::SubscriptionSet>(
        *ctx->message_subscriptions);
    updated->erase(address ? address : "");
    ctx->message_subscriptions = std::move(updated);
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status pointreceiver_dequeue_message(
    pointreceiver_context *ctx, char *out_address, size_t address_capacity,
    pointreceiver_message *out_message, int timeout_ms) {
  using namespace pc::receiver;

  if (!ctx || !out_message) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;

  return exception_boundary("pointreceiver_dequeue_message", [&] {
    MessageFrame frame;
    if (!ctx->message_queue.wait_dequeue_timed(frame,
                                               milliseconds(timeout_ms))) {
      return POINTRECEIVER_ERROR_TIMEOUT;
    }
    const auto &[address, value_variant] = frame;

    // pass out the address
    copy_to_buffer(out_address, address_capacity, address);

    // and pass out the message structure, filling the union
    if (const auto *bool_value = std::get_if<bool>(&value_variant)) {
      out_message->value_type = POINTRECEIVER_MESSAGE_VALUE_BOOL;
      out_message->value.bool_val = *bool_value;
    } else if (const auto *int_value = std::get_if<int>(&value_variant)) {
      out_message->value_type = POINTRECEIVER_MESSAGE_VALUE_INT;
      out_message->value.int_val = *int_value;
    } else if (const auto *float_value = std::get_if<float>(&value_variant)) {
      out_message->value_type = POINTRECEIVER_MESSAGE_VALUE_FLOAT;
      out_message->value.float_val = *float_value;
    } else {
      out_message->value_type = POINTRECEIVER_MESSAGE_VALUE_UNKNOWN;
      return POINTRECEIVER_ERROR_DECODE_FAILED;
    }
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status pointreceiver_start_point_receiver(
    pointreceiver_context *ctx, const char *pointcaster_point_cloud_address) {
  if (!ctx || !pointcaster_point_cloud_address) {
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  }
  if (ctx->point_cloud_worker.joinable()) {
    return POINTRECEIVER_ERROR_ALREADY_RUNNING;
  }

  return exception_boundary("pointreceiver_start_point_receiver", [&] {
    const std::string endpoint(pointcaster_point_cloud_address);
    zmq::socket_t socket(*ctx->zmq_ctx, zmq::socket_type::sub);

    try {
      // TODO why 32 msgs? what is an actually valid number here for the
      // high watermark?
      socket.set(zmq::sockopt::rcvhwm, 32);
      socket.set(zmq::sockopt::linger, 0);
      constexpr auto receive_thread_block_timeout_ms = 100;
      socket.set(zmq::sockopt::rcvtimeo, receive_thread_block_timeout_ms);
      socket.connect(endpoint);
    } catch (const zmq::error_t &e) {
      pc::logger()->error("Point receiver failed to connect to '{}' - {}",
                          endpoint, e.what());
      return POINTRECEIVER_ERROR_CONNECTION_FAILED;
    }

    pc::logger()->info("Point receiver connected to {}", endpoint);

    ctx->point_cloud_stopping.store(false, std::memory_order_release);
    ctx->point_cloud_worker =
        std::thread([ctx, socket = std::move(socket)]() mutable {
          exception_boundary("Point cloud receive thread", [&] {
            pc::receiver::point_cloud_receive_loop(*ctx, std::move(socket));
            return POINTRECEIVER_OK;
          });
        });
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status
pointreceiver_stop_point_receiver(pointreceiver_context *ctx) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  if (!ctx->point_cloud_worker.joinable()) {
    return POINTRECEIVER_ERROR_NOT_RUNNING;
  }
  return exception_boundary("pointreceiver_stop_point_receiver", [&] {
    pc::logger()->trace("Stopping point receiver");
    ctx->stop_point_cloud_worker();
    {
      std::lock_guard lock(ctx->point_cloud_stream_mutex);
      ctx->point_cloud_frames.clear();
    }
    pc::logger()->info("Point receiver thread ended");
    return POINTRECEIVER_OK;
  });
}

bool pointreceiver_point_receiver_running(pointreceiver_context *ctx) {
  return ctx && ctx->point_cloud_worker.joinable();
}

pointreceiver_status
pointreceiver_subscribe_to_point_cloud(pointreceiver_context *ctx,
                                       const char *address) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_subscribe_to_point_cloud", [&] {
    std::lock_guard lock(ctx->point_cloud_subscription_mutex);
    auto updated = std::make_shared<pc::receiver::SubscriptionSet>(
        *ctx->point_cloud_subscriptions);
    updated->insert(address ? address : "");
    ctx->point_cloud_subscriptions = std::move(updated);
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status
pointreceiver_unsubscribe_from_point_cloud(pointreceiver_context *ctx,
                                           const char *address) {
  if (!ctx) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_unsubscribe_from_point_cloud", [&] {
    std::lock_guard lock(ctx->point_cloud_subscription_mutex);
    auto updated = std::make_shared<pc::receiver::SubscriptionSet>(
        *ctx->point_cloud_subscriptions);
    updated->erase(address ? address : "");
    ctx->point_cloud_subscriptions = std::move(updated);
    return POINTRECEIVER_OK;
  });
}

pointreceiver_status pointreceiver_dequeue_point_cloud(
    pointreceiver_context *ctx, char *out_address, size_t address_capacity,
    pointreceiver_point_cloud_frame *out_frame, int timeout_ms) {
  using namespace pc::receiver;

  if (!ctx || !out_frame || !out_address || address_capacity == 0) {
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  }

  return exception_boundary("pointreceiver_dequeue_point_cloud", [&] {
    std::unique_lock point_cloud_stream_access(ctx->point_cloud_stream_mutex);

    auto oldest_pending = ctx->point_cloud_frames.end();

    ctx->point_cloud_stream_cv.wait_for(
        point_cloud_stream_access, milliseconds(timeout_ms), [&] {
          auto pending_frames = ctx->point_cloud_frames |
                                std::views::filter([](const auto &frame_entry) {
                                  return frame_entry.second.pending;
                                });
          const auto oldest = std::ranges::min_element(
              pending_frames, {}, [](const auto &frame_entry) {
                return frame_entry.second.timestamp;
              });
          if (oldest == pending_frames.end()) return false;
          oldest_pending = oldest.base();
          return true;
        });

    if (oldest_pending == ctx->point_cloud_frames.end()) {
      return POINTRECEIVER_ERROR_TIMEOUT;
    }

    PointCloudFrame &frame = oldest_pending->second;
    frame.pending = false;
    frame.current_cloud = std::move(frame.incoming_cloud);
    const auto cloud = frame.current_cloud;

    auto &attributes = frame.current_attributes;
    attributes.clear();
    attributes.reserve(cloud->attributes.size());

    for (const auto &[name, attribute] : cloud->attributes) {
      // a float attribute keeps the default identity step and offset
      const auto &encoding = attribute.encoding;
      std::visit(
          [&](const auto &values) {
            using element = typename std::decay_t<decltype(values)>::value_type;

            pointreceiver_attribute descriptor{
                .name = name.c_str(),
                .data = values.data(),
                .element_count = values.size(),
                .quantisation_step = encoding.step,
                .quantisation_offset = encoding.offset,
                .component_count = 1,
                .stride = static_cast<uint32_t>(sizeof(element))};

            if constexpr (std::is_arithmetic_v<element>) {
              descriptor.element_type = wire_type<element>();

            } else if constexpr (requires(element value) {
                                   value.x;
                                   value.y;
                                 }) {
              // vectors like float3, int2 etc.
              using component = decltype(element::x);
              descriptor.element_type = wire_type<component>();
              descriptor.component_count =
                  static_cast<uint32_t>(sizeof(element) / sizeof(component));
            } else {
              static_assert(false, "attribute element has no description");
            }

            attributes.push_back(descriptor);
          },
          attribute.storage);
    }

    std::ranges::sort(attributes, {}, [](const auto &attribute) {
      return std::string_view{attribute.name};
    });

    const auto *attributes_ptr = attributes.data();
    const auto attribute_count = attributes.size();
    copy_to_buffer(out_address, address_capacity, oldest_pending->first);

    point_cloud_stream_access.unlock();

    out_frame->point_count = cloud->size();
    out_frame->positions = reinterpret_cast<const pointreceiver_position_t *>(
        cloud->positions.data());
    out_frame->colours =
        reinterpret_cast<const pointreceiver_color_t *>(cloud->colors.data());
    out_frame->attributes = attributes_ptr;
    out_frame->attribute_count = attribute_count;
    return POINTRECEIVER_OK;
  });
}

const pointreceiver_attribute *
pointreceiver_find_attribute(const pointreceiver_point_cloud_frame *frame,
                             const char *name) {
  if (!frame || !frame->attributes || !name) return nullptr;

  const std::string_view wanted{name};
  for (size_t i = 0; i < frame->attribute_count; i++) {
    if (std::string_view{frame->attributes[i].name} == wanted) {
      return &frame->attributes[i];
    }
  }
  return nullptr;
}

pointreceiver_status
pointreceiver_attribute_copy_floats(const pointreceiver_attribute *attribute,
                                    float *out, size_t out_capacity) {
  if (!attribute || !attribute->data || !out) {
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  }

  const auto component_count = static_cast<size_t>(attribute->component_count);
  const auto value_count = attribute->element_count * component_count;
  if (out_capacity < value_count) return POINTRECEIVER_ERROR_INVALID_ARGUMENT;

  const auto *bytes = static_cast<const std::byte *>(attribute->data);
  const auto step = attribute->quantisation_step;
  const auto offset = attribute->quantisation_offset;

  // an unquantised float attribute can be copied raw without encoding
  if (attribute->element_type == POINTRECEIVER_ATTRIBUTE_FLOAT32 &&
      step == 1.0f && offset == 0.0f &&
      attribute->stride == sizeof(float) * component_count) {
    std::memcpy(out, bytes, value_count * sizeof(float));
    return POINTRECEIVER_OK;
  }

  const auto copy_as = [&]<typename Scalar>(std::type_identity<Scalar>) {
    for (size_t i = 0; i < attribute->element_count; i++) {
      const auto *element =
          reinterpret_cast<const Scalar *>(bytes + i * attribute->stride);
      for (size_t component = 0; component < component_count; component++) {
        out[i * component_count + component] =
            static_cast<float>(element[component]) * step + offset;
      }
    }
  };

  switch (attribute->element_type) {
  case POINTRECEIVER_ATTRIBUTE_FLOAT32:
    copy_as(std::type_identity<float>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_UINT8:
    copy_as(std::type_identity<std::uint8_t>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_INT8:
    copy_as(std::type_identity<std::int8_t>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_UINT16:
    copy_as(std::type_identity<std::uint16_t>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_UINT32:
    copy_as(std::type_identity<std::uint32_t>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_INT16:
    copy_as(std::type_identity<std::int16_t>{});
    break;
  case POINTRECEIVER_ATTRIBUTE_INT32:
    copy_as(std::type_identity<std::int32_t>{});
    break;
  default:
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  }
  return POINTRECEIVER_OK;
}

size_t
pointreceiver_known_point_cloud_address_count(pointreceiver_context *ctx) {
  if (!ctx) return 0;
  std::lock_guard lock(ctx->point_cloud_stream_mutex);
  return ctx->known_point_cloud_addresses.size();
}

pointreceiver_status pointreceiver_get_known_point_cloud_address(
    pointreceiver_context *ctx, size_t index, char *out, size_t out_capacity) {
  if (!ctx || !out || out_capacity == 0)
    return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
  return exception_boundary("pointreceiver_get_known_point_cloud_address", [&] {
    std::lock_guard lock(ctx->point_cloud_stream_mutex);
    if (index >= ctx->known_point_cloud_addresses.size()) {
      return POINTRECEIVER_ERROR_OUT_OF_RANGE;
    }
    const auto it = std::next(ctx->known_point_cloud_addresses.begin(),
                              static_cast<std::ptrdiff_t>(index));
    if (!pc::receiver::copy_to_buffer(out, out_capacity, *it))
      return POINTRECEIVER_ERROR_INVALID_ARGUMENT;
    return POINTRECEIVER_OK;
  });
}
}
