#include "osc_sender.h"
#include "pointcaster/point_cloud.h"
#include "publishers/osc/osc_sender_config.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <format>
#include <logger/logger.h>
#include <memory>
#include <mutex>
#include <profiling/profiling_zone.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <workspace/workspace.h>

#include <lo/lo.h>
#define LO_USE_EXCEPTIONS
#include <lo/lo_cpp.h>

namespace pc::publishers {

using namespace pc::profiling;

namespace {

using PositionEncoding = OscSenderConfiguration::PositionEncoding;

// config paths are already '/' delimited like osc addresses, they just don't
// carry the leading slash an address needs
std::string osc_address_from(std::string_view path) {
  if (path.starts_with('/')) return std::string(path);
  return std::format("/{}", path);
}

} // namespace

class OscConnection {
public:
  explicit OscConnection(const OscSenderConfiguration &config)
      : _address(config.host, config.port) {
    pc::logger()->info("OSC sender publishing to '{}:{}'", _address.hostname(),
                       _address.port());
  }

  OscConnection(const OscConnection &) = delete;
  OscConnection &operator=(const OscConnection &) = delete;
  OscConnection(OscConnection &&) = delete;
  OscConnection &operator=(OscConnection &&) = delete;

  void send(const std::string &address, const lo::Message &out) const {
    ProfilingZone osc_send_zone("OscSender::send");
    try {
      if (_address.send(address, out) == -1) {
        pc::logger()->error("OSC send to '{}' failed: {}", address,
                            _address.errstr());
      }
    } catch (const lo::Error &) {
      pc::logger()->error("OSC send to '{}' failed with a liblo error",
                          address);
    } catch (const lo::Invalid &) {
      pc::logger()->error("OSC send to '{}' used an invalid liblo object",
                          address);
    }
  }

  bool matches(const OscSenderConfiguration &config) const {
    return _address.hostname() == config.host &&
           _address.port() == std::to_string(config.port);
  }

private:
  lo::Address _address;
};

OscSender::OscSender(Workspace &workspace)
    : _listener(*this, workspace, workspace.config_registry) {}

OscSender::~OscSender() = default;

void OscSender::handle_config_change(
    std::string_view path, const OscSenderConfiguration &config_snapshot) {

  if (!config_snapshot.enabled) {
    _connection.reset();
    return;
  }
  if (_connection && _connection->matches(config_snapshot)) return;

  _connection.reset();
  try {
    _connection = std::make_unique<OscConnection>(config_snapshot);
  } catch (const lo::Error &) {
    pc::logger()->error("OSC sender failed to resolve '{}:{}'",
                        config_snapshot.host, config_snapshot.port);
  }
}

void OscSender::handle_update(const std::string_view path,
                              const ConfigValue &value,
                              const OscSenderConfiguration &config_snapshot) {

  if (!_connection) return;

  const auto &position_encoding = config_snapshot.position_encoding;
  const auto max_cloud_points = std::max(1, config_snapshot.max_cloud_points);

  // positions can go out as either raw millimeter ints or floats as metres
  const auto push_back_position = [&](lo::Message &out, const position &p) {
    if (position_encoding == PositionEncoding::Metres) {
      out.add_float(length{p.x}.metres());
      out.add_float(length{p.y}.metres());
      out.add_float(length{p.z}.metres());
    } else {
      out.add_int32(p.x);
      out.add_int32(p.y);
      out.add_int32(p.z);
    }
  };

  // OSC requires a point cloud to be sent as
  // individual elements, it has no array type:
  const auto send_cloud_elements = [&](std::string_view base, std::size_t count,
                                       auto &&fill_message) {
    const auto address = osc_address_from(base);
    const auto sent_count =
        std::min(count, static_cast<std::size_t>(max_cloud_points));

    {
      lo::Message out;
      out.add_int32(static_cast<int32_t>(sent_count));
      _connection->send(std::format("{}/count", address), out);
    }

    for (std::size_t i = 0; i < sent_count; i++) {
      lo::Message out;
      fill_message(out, i);
      _connection->send(std::format("{}/{}", address, i), out);
    }

    if (sent_count < count) {
      pc::logger()->debug(
          "OSC truncated '{}' to {} of {} points (max_cloud_points)", address,
          sent_count, count);
    }
  };

  try {
    std::visit(
        [&](auto &v) {
          ProfilingZone osc_zone("OscSender::process_msg");

          using VariantType = std::decay_t<decltype(v)>;

          if constexpr (std::same_as<VariantType, AabbListPtr>) {
            if (!v) return;
            // each aabb is one message of its min followed by its max
            send_cloud_elements(
                path, v->min_positions().size(),
                [&](lo::Message &out, std::size_t i) {
                  push_back_position(out, v->min_positions()[i]);
                  push_back_position(out, v->max_positions()[i]);
                });
          } else if constexpr (pc::is_cloud_stream_v<VariantType>) {
            if (!v) return;
            send_cloud_elements(path, v->positions.size(),
                                [&](lo::Message &out, std::size_t i) {
                                  push_back_position(out, v->positions[i]);
                                });
          } else if constexpr (std::same_as<VariantType, position_bounds>) {
            // a bounds is one box: its min followed by its max, the same
            // message a single element of an aabb list sends
            lo::Message out;
            {
              ProfilingZone osc_encode_zone("OscSender::encode");
              push_back_position(out, v.min);
              push_back_position(out, v.max);
            }
            _connection->send(osc_address_from(path), out);
          } else {
            lo::Message out;
            {
              ProfilingZone osc_encode_zone("OscSender::encode");
              if constexpr (std::same_as<VariantType, bool>) {
                out.add_bool(v);
              } else if constexpr (std::same_as<VariantType, radius> ||
                                   std::same_as<VariantType, length>) {
                out.add_int32(static_cast<int32_t>(v.mm));
              } else if constexpr (std::same_as<VariantType, std::string>) {
                out.add_string(v);
              } else if constexpr (std::same_as<VariantType, float>) {
                out.add_float(v);
              } else if constexpr (std::same_as<VariantType, double>) {
                out.add_double(v);
              } else if constexpr (std::is_integral_v<VariantType>) {
                out.add_int32(static_cast<int32_t>(v));
              } else {
                out.add_string(std::format("{}", v));
              }
            }
            _connection->send(osc_address_from(path), out);
          }
        },
        value);
  } catch (const lo::Error &) {
    pc::logger()->error("OSC failed to build a message for '{}'", path);
  } catch (const lo::Invalid &) {
    pc::logger()->error("OSC failed to build a message for '{}'", path);
  }
}

OscSenderConfiguration OscSender::config(Workspace &workspace) const {
  std::scoped_lock lock(workspace.config_access);
  return workspace.config.publishers.osc;
}

} // namespace pc::publishers
