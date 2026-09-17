#include "osc_receiver.h"

#include "config/config_registry.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config.h"

#include <core/logger/logger.h>
#include <lo/lo.h>
#define LO_USE_EXCEPTIONS
#include <lo/lo_cpp.h>

#include <algorithm>
#include <cstring>

namespace pc::receivers {

OscReceiver::OscReceiver(Workspace &workspace) : _workspace(workspace) {
  reconfigure();
}

OscReceiver::~OscReceiver() {
  stop();
}

OscReceiverConfiguration &OscReceiver::config() {
  return _workspace.config.osc_receiver;
}

void OscReceiver::reconfigure() {
  const bool should_run = config().enable;
  const int port = std::clamp(config().port, 1024, 49151);

  if (_server && (!should_run || port != _active_port)) stop();
  if (!_server && should_run) start(port);
}

void OscReceiver::start(int port) {
  try {

    constexpr auto log_liblo_error = [](int num, const char *msg,
                                        const char *where) {
      pc::logger()->error("liblo error {}: {} ({})", num, msg ? msg : "unknown",
                          where ? where : "");
    };

    _server = std::make_unique<lo::ServerThread>(port, log_liblo_error);

    const auto dispatch_osc = [this](const char *path, const char *types,
                                     lo_arg **argv, int /*argc*/) {
      dispatch(path, types, reinterpret_cast<void **>(argv));
    };
    constexpr auto osc_receive_path = nullptr;
    constexpr auto osc_receive_types = nullptr;

    _server->add_method(osc_receive_path, osc_receive_types, dispatch_osc);

    _server->start();
    _active_port = port;
    _state = State::Running;
    pc::logger()->info("OSC receiver listening on port {}", port);

  } catch (const lo::Error &) {
    _server.reset();
    _state = State::Failed;
    pc::logger()->error(
        "OSC receiver failed to start on port {} (is port in use?)", port);
  }
}

void OscReceiver::stop() {
  if (!_server) return;
  _server->stop();
  _server.reset();
  _state = State::Stopped;
  pc::logger()->info("OSC receiver stopped");
}

void OscReceiver::dispatch(const char *address, const char *types,
                           void **argv) {
  if (!address || !types || address[0] != '/') return;

  pc::logger()->trace("received OSC at: {}", address);

  // strip leading '/'
  const std::string_view path{address + 1};
  auto *args = reinterpret_cast<lo_arg **>(argv);

  ConfigValue value;
  bool matched = true;

  if (std::strcmp(types, "f") == 0) {
    value = args[0]->f;
  } else if (std::strcmp(types, "i") == 0) {
    value = args[0]->i;
  } else if (std::strcmp(types, "d") == 0) {
    value = static_cast<float>(args[0]->d);
  } else if (std::strcmp(types, "s") == 0) {
    value = std::string{&args[0]->s};
  } else if (std::strcmp(types, "T") == 0) {
    value = true;
  } else if (std::strcmp(types, "F") == 0) {
    value = false;
  } else {
    matched = false;
  }

  if (!matched) {
    pc::logger()->debug("OSC unhandled type '{}' at '{}'", types, address);
    return;
  }

  if (!_workspace.config_registry.set(path, value)) {
    pc::logger()->debug("OSC no field registered for '{}'", path);
  }
}

} // namespace pc::receivers
