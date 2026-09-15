#include "orbbec_device.h"
#include "orbbec_context.h"
#include "orbbec_device_config.h"
#include "orbbec_utils.h"

#include <Corrade/Containers/Pointer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <core/logger/logger.h>
#include <core/profiling/profiling_zone.h>
#include <cstdint>
#include <cstring>
#include <execution>
#include <filesystem>
#include <format>
#include <libobsensor/ObSensor.hpp>
#include <libobsensor/h/ObTypes.h>
#include <libobsensor/hpp/Context.hpp>
#include <libobsensor/hpp/Error.hpp>
#include <libobsensor/hpp/Frame.hpp>
#include <libobsensor/hpp/Pipeline.hpp>
#include <libobsensor/hpp/Utils.hpp>
#include <limits>
#include <memory>
#include <metrics/metrics.h>
#include <mutex>
#include <numbers>
#include <numeric>
#include <plugins/backend/backend_utils.h>
#include <plugins/devices/device_tree.h>
#include <plugins/devices/device_variants.h>
#include <pointcaster/point_cloud.h>
#include <profiling/profiler.h>
#include <random>
#include <ranges>
#include <span>
#include <thread>
#include <util/color_utils.h>
#include <util/string_utils.h>
#include <variant>
#include <workspace/workspace.h>

#include <plugins/backend/cpu/cpu_backend.h>
#include <pointcaster/task_pool.h>
#include <rfl/Variant.hpp>

#ifndef POINTCASTER_ORBBEC_SDK_VERSION
#error "POINTCASTER_ORBBEC_SDK_VERSION must be defined (1 or 2) by the build"
#endif

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace pc::backend;
using pc::profiling::ProfilingZone;

namespace {
std::atomic_bool does_have_discovery_change_callback = false;
} // namespace

namespace pc::devices {

OrbbecDevice::OrbbecDevice(Corrade::PluginManager::AbstractManager &manager,
                           Corrade::Containers::StringView plugin)
    : DevicePlugin(manager, plugin) {
  pc::logger()->trace("Creating new OrbbecDevice");
  try {
    orbbec_context().retain_user();
    orbbec_context().discover_devices_async();
  } catch (...) {
    pc::logger()->error("Exception during Orbbec context initialisation");
  }
  _timeout_thread = std::jthread([this](auto stop_token) {
    // &OrbbecDevice::timeout_thread_work
    timeout_thread_work(stop_token);
  });
  pc::logger()->trace("Created OrbbecDevice");
}

OrbbecDevice::~OrbbecDevice() {
  shutdown();
  pc::logger()->trace("Destroyed OrbbecDevice{}",
                      _is_discovery_instance ? " discovery instance" : "");
}

void OrbbecDevice::shutdown() {
  if (_shutdown_complete.exchange(true)) return;
  pc::logger()->trace("Destroying OrbbecDevice{}",
                      _is_discovery_instance ? " discovery instance" : "");
  _initialisation_thread.request_stop();
  if (_initialisation_thread.joinable()) _initialisation_thread.join();
  try {
    stop_sync();
  } catch (const std::exception &e) {
    pc::logger()->error("Exception closing OrbbecDevice: {}", e.what());
  } catch (...) {
    pc::logger()->error("Unknown exception closing OrbbecDevice");
  }
  _timeout_thread.request_stop();
  if (_timeout_thread.joinable()) _timeout_thread.join();
  orbbec_context().release_user();
}

std::vector<DiscoveredDevice> OrbbecDevice::discovered_devices() const {
  std::vector<DiscoveredDevice> out;

  auto &ctx = orbbec_context();
  std::lock_guard lock(ctx.discovered_devices_access);

  const static std::map<std::string, std::string> product_type_label = {
      {"Orbbec Femto Mega", "rgbd"}, {"Orbbec LiDAR SL450", "lidar"}};

  out.reserve(ctx.discovered_devices.size());
  for (const auto &d : ctx.discovered_devices) {
    // network devices are identified by ip, usb devices by connection type
    const auto &location = d.is_network() ? d.ip : d.connection_type;
    pc::logger()->debug("name: {}, product id: {}", d.name, d.product_id);
    out.push_back(
        DiscoveredDevice{.label = std::format("{} ({})", d.name, location),
                         .ip = d.ip,
                         .id = d.id,
                         .type_label = product_type_label.at(d.name)});
  }

  return out;
}

void OrbbecDevice::refresh_discovery() {
  orbbec_context().discover_devices_async();
}

void OrbbecDevice::add_discovery_change_callback(std::function<void()> cb) {
  orbbec_context().add_discovery_change_callback(std::move(cb));
  does_have_discovery_change_callback = true;
}

bool OrbbecDevice::has_discovery_change_callback() const {
  return does_have_discovery_change_callback.load();
}

std::vector<PluginSettingsPage> OrbbecDevice::settings_pages() const {
  // the qml ships one level above the sdk-version variant directory
  const auto qml_file_path =
      plugin_directory().parent_path() / "OrbbecSettingsPage.qml";
  return {{.key = "orbbec",
           .title = "Orbbec",
           .qml_file_path = qml_file_path.string()}};
}

DeviceStatus OrbbecDevice::status() const {
  if (_in_error_state) return DeviceStatus::Missing;
  if (_loading_pipeline) return DeviceStatus::Loading;
  auto ctx = orbbec_context().get_if_ready();
  if (!ctx) return DeviceStatus::Unloaded;
  if (!_running_pipeline) {
    return DeviceStatus::Loaded;
  }
  if (_last_updated_time.load() == std::chrono::steady_clock::time_point{}) {
    return DeviceStatus::Loaded;
  }
  return DeviceStatus::Active;
}

void OrbbecDevice::start() {
  pc::logger()->trace("Running 'start()'");
  _initialisation_thread = std::jthread([this](std::stop_token) {
    // TODO stop_token is currently unused,
    // should pass it in and check at different initialisation thread steps
    start_sync();
  });
}

void OrbbecDevice::stop() {
  pc::logger()->trace("Running 'stop()'");
  _initialisation_thread = std::jthread([this](std::stop_token) {
    // TODO stop_token is currently unused,
    // should pass it in and check at different initialisation thread steps
    stop_sync();
  });
}

void OrbbecDevice::restart() {
  pc::logger()->trace("Running 'restart()'");
  _initialisation_thread = std::jthread([this](std::stop_token) {
    // TODO stop_token is currently unused,
    // should pass it in and check at different initialisation thread steps
    stop_sync();
    start_sync();
  });
}

void OrbbecDevice::start_sync() {
  pc::logger()->trace("Attempting to start an Orrbec driver");

  std::lock_guard lock(orbbec_context().device_api_access);

  set_loading(true);
  set_running(false);
  set_error_state(false);
  set_updated_time(steady_clock::time_point{});

  auto ob_ctx = orbbec_context().wait_til_ready();
  if (!ob_ctx) {
    pc::logger()->trace(
        "Orbbec context not ready in time, aborting start_sync()");
    set_error_state(true);
    set_loading(false);
    return;
  }

  // if this plugin instance is only for context initialisation and device
  // discovery, dont bother trying to attach to a device
  if (is_discovery_instance()) {
    set_loading(false);
    return;
  }

  auto config = std::get<OrbbecDeviceConfiguration>(this->config_variant());
  pc::logger()->info("Initialising OrbbecDevice ({})", config.id);

  std::shared_ptr<ob::Device> ob_device;

  // try to find existing device in the list by device serial num
  if (!config.serial.empty()) {
    auto ob_device_list = ob_ctx->queryDeviceList();
    const uint32_t device_count = ob_device_list->deviceCount();
    for (uint32_t i = 0; i < device_count; ++i) {
      auto serial = ob_device_list->serialNumber(i);
      if (std::strcmp(serial, config.serial.c_str()) == 0) {
        try {
          ob_device = ob_device_list->getDevice(i);
          pc::logger()->trace("Completed ob_device_list->getDevice({})", i);
        } catch (...) {
          pc::logger()->warn("Unable to open device with matched serial");
          pc::logger()->warn("Check connection to camera");
        }
        break;
      }
    }
  }

  const bool is_lidar = rfl::holds_alternative<
      OrbbecDeviceConfiguration::LidarSensorConfiguration>(
      config.sensor.value().variant());

  const uint16_t net_device_port = is_lidar ? 2228 : 8090;

  const auto &network_config = config.network.value();
  const auto &ip = network_config.ip_address.value();

  bool connected_using_ip = false;
  if (!ob_device && !ip.empty()) {
    try {
      pc::logger()->trace("Attempting createNetDevice at {}:{}", ip,
                          net_device_port);
      ob_device = ob_ctx->createNetDevice(ip.c_str(), net_device_port);
      connected_using_ip = true;
    } catch (const ob::Error &e) {
      pc::logger()->error("Failed to create OrbbecDevice at {}:{} {}",
                          network_config.ip_address.value(), net_device_port,
                          e.getMessage());
      set_error_state(true);
      set_loading(false);
      return;
    } catch (...) {
      pc::logger()->error("Unknown error creating Orbbec NetDevice at {}:{}",
                          network_config.ip_address.value(), net_device_port);
      set_error_state(true);
      set_loading(false);
      return;
    }
  }

  if (!ob_device) {
    pc::logger()->warn("Unable to find Orbbec at {}:{}", ip, net_device_port);
    set_error_state(true);
    set_loading(false);
    return;
  }

  const auto device_info = ob_device->getDeviceInfo();
  bool config_changed = false;

  if (config.serial.empty() || connected_using_ip) {
    config.serial = device_info->serialNumber();
    config_changed = true;
  }

  // the sdk hands back 0.0.0.0 rather than nothing when it has no ip
  constexpr auto unassigned_ip = "0.0.0.0";
  const std::string device_ip = device_info->ipAddress();
  if (device_ip != unassigned_ip && ip != device_ip) {
    pc::logger()->warn("OrbbecDevice ({}) is at {}, was {}", config.id,
                       device_ip, ip.empty() ? "unset" : ip);
    config.network.value().ip_address.set(device_ip);
    config_changed = true;
  }

  if (config_changed) update_config(config);

  pc::logger()->trace("Successfully created device at {}:{}", ip,
                      net_device_port);

  _pipeline_thread =
      std::jthread([this, ob_device = std::move(ob_device)](auto stop_token) {
        pipeline_thread_work(stop_token, ob_device);
      });

  set_running(true);
  set_error_state(false);
}

void OrbbecDevice::stop_sync() {
  pc::logger()->trace("Running 'stop_sync()'");
  if (!_running_pipeline) return;
  auto config = std::get<OrbbecDeviceConfiguration>(this->config_variant());
  std::lock_guard lock(orbbec_context().device_api_access);
  if (!_pipeline_thread.joinable()) return;
  pc::logger()->info("Closing OrbbecDevice {}",
                     config.network.value().ip_address.value());
  pc::logger()->trace("Joining pipeline thread");
  _pipeline_thread.request_stop();
  _pipeline_thread.join();
  pc::logger()->trace("Pipeline thread complete");
  set_running(false);
  set_updated_time(steady_clock::time_point{});
}

void OrbbecDevice::set_updated_time(
    std::chrono::steady_clock::time_point new_time) {
  const auto old_time = _last_updated_time.exchange(new_time);

  const bool new_invalid =
      (new_time == std::chrono::steady_clock::time_point{});
  const bool old_invalid =
      (old_time == std::chrono::steady_clock::time_point{});

  if (!new_invalid) {
    const auto prev_tick = _pipeline_last_tick.exchange(new_time);

    if (prev_tick != std::chrono::steady_clock::time_point{}) {
      const std::chrono::duration<float> dt = new_time - prev_tick;
      if (dt.count() > 0.0f) {
        static constexpr float pipeline_fps_ema_alpha = 0.10f;

        const float inst_fps = 1.0f / dt.count();
        const float old_ema = _pipeline_fps_ema.load();
        const float new_ema = (old_ema == 0.0f)
                                  ? inst_fps
                                  : (pipeline_fps_ema_alpha * inst_fps +
                                     (1.0f - pipeline_fps_ema_alpha) * old_ema);
        _pipeline_fps_ema.store(new_ema);
      }
    }
  } else {
    _pipeline_last_tick.store(std::chrono::steady_clock::time_point{});
    _pipeline_fps_ema.store(0.0f);
  }

  if (new_invalid != old_invalid) notify_status_changed();
}

void OrbbecDevice::pipeline_thread_work(std::stop_token stop_token,
                                        std::shared_ptr<ob::Device> ob_device) {
  // the sensor configuration held by the device decides which set of api
  // calls drive the pipeline, so each sensor type get their own worker branch
  const auto &device_config = std::get<OrbbecDeviceConfiguration>(_config);

  device_config.sensor.value().visit([&](const auto &sensor_config) {
    using SensorConfiguration = std::decay_t<decltype(sensor_config)>;

    if constexpr (std::same_as<
                      SensorConfiguration,
                      OrbbecDeviceConfiguration::RgbdSensorConfiguration>) {
      rgbd_pipeline_thread_work(std::move(stop_token), std::move(ob_device));
    } else if constexpr (std::same_as<SensorConfiguration,
                                      OrbbecDeviceConfiguration::
                                          LidarSensorConfiguration>) {
      lidar_pipeline_thread_work(std::move(stop_token), std::move(ob_device));
    }
  });
}

void OrbbecDevice::rgbd_pipeline_thread_work(
    std::stop_token stop_token, std::shared_ptr<ob::Device> ob_device) {
  try {
    pc::logger()->trace("creating new orbbec rgbd pipeline");

    // we get exclusive access to the orbbec device api during initialisation
    // of this thread, but unlock before starting the working loop. this
    // ensures that devices are loaded serially because the sdk is not thread
    // safe
    std::unique_lock<std::mutex> ob_device_api_access(
        orbbec_context().device_api_access);

    ob::Pipeline pipeline{ob_device};

    auto ob_config = std::make_shared<ob::Config>();

    auto &device_config = std::get<OrbbecDeviceConfiguration>(_config);
    auto &sensor_config =
        rfl::get<OrbbecDeviceConfiguration::RgbdSensorConfiguration>(
            device_config.sensor.get().variant());

    const auto [colour_width, colour_height] =
        orbbec::resolution(sensor_config.color_resolution.value());
    const auto [depth_width, depth_height] =
        orbbec::resolution(sensor_config.depth_resolution.value());

    auto colour_profile_list = pipeline.getStreamProfileList(OB_SENSOR_COLOR);
    // TODO enable without colour too
    if (!colour_profile_list) {
      pc::logger()->error("No Orbbec colour profiles available");
      set_error_state(true);
      return;
    }
    std::shared_ptr<ob::VideoStreamProfile> colour_profile;
    try {
      colour_profile = colour_profile_list->getVideoStreamProfile(
          colour_width, colour_height, OB_FORMAT_RGB, 30);
    } catch (const ob::Error &e) {
      pc::logger()->error("Failed to get colour profile: {}", e.getMessage());
      set_error_state(true);
      return;
    }
    ob_config->enableStream(colour_profile);

    std::shared_ptr<ob::VideoStreamProfile> depth_profile;

    if (sensor_config.conversion_mode.value() ==
        OrbbecDeviceConfiguration::PointConversionMode::D2C) {

      // try hardware D2C first
      std::shared_ptr<ob::StreamProfileList> depth_d2c_list;
      try {
        depth_d2c_list =
            pipeline.getD2CDepthProfileList(colour_profile, ALIGN_D2C_HW_MODE);
        if (depth_d2c_list && depth_d2c_list->count() > 0) {
          depth_profile = depth_d2c_list->getVideoStreamProfile(
              depth_width, depth_height, OB_FORMAT_Y16, 30);
          ob_config->setAlignMode(ALIGN_D2C_HW_MODE);
        }
      } catch (const ob::Error &e) {
        pc::logger()->warn("Failed to get HW D2C depth profile list: {}",
                           e.getMessage());
      }
      // if that didn't work, go to software D2C
      if (!depth_profile) {
        try {
          depth_d2c_list = pipeline.getD2CDepthProfileList(colour_profile,
                                                           ALIGN_D2C_SW_MODE);
          if (depth_d2c_list && depth_d2c_list->count() > 0) {
            depth_profile = depth_d2c_list->getVideoStreamProfile(
                depth_width, depth_height, OB_FORMAT_Y16, 30);
            ob_config->setAlignMode(ALIGN_D2C_SW_MODE);
          }
        } catch (const ob::Error &e) {
          pc::logger()->warn("Failed to get SW D2C depth profile list: {}",
                             e.getMessage());
        }
      }

      // if that didn't work, D2C is unavailable
      if (!depth_profile) {
        set_error_state(true);
        throw std::format_error("Device does not support D2C conversion mode");
      }

    } else if (sensor_config.conversion_mode.value() ==
               OrbbecDeviceConfiguration::PointConversionMode::C2D) {
      // need to do stuff here
    }

    // sensor_config aliases _config, so this lands on the plugin's own copy
    // directly. fps is rfl::Skip, so there is nothing to persist upstream
    const auto fps = std::min(depth_profile->fps(), colour_profile->fps());
    sensor_config.fps.set(fps);

    pc::logger()->trace("enabling stream at: {} fps", fps);

    ob_config->enableStream(depth_profile);
    // ob_config->setFrameAggregateOutputMode(
    //     OBFrameAggregateOutputMode::
    //         OB_FRAME_AGGREGATE_OUTPUT_ALL_TYPE_FRAME_REQUIRE);

    // this is frame sync between the devices own depth and colour cameras
    pipeline.enableFrameSync();

    // this is frame sync between devices
    if (sensor_config.sync_mode.value() ==
        OrbbecDeviceConfiguration::SyncMode::Software) {
      OBMultiDeviceSyncConfig ob_sync_config{};
      ob_sync_config.syncMode = OB_MULTI_DEVICE_SYNC_MODE_SOFTWARE_TRIGGERING;
      ob_device->setMultiDeviceSyncConfig(ob_sync_config);
      orbbec_context().add_to_software_sync_list(ob_device);
    } else if (sensor_config.sync_mode.value() ==
               OrbbecDeviceConfiguration::SyncMode::Standalone) {
      OBMultiDeviceSyncConfig ob_sync_config{};
      ob_sync_config.syncMode = OB_MULTI_DEVICE_SYNC_MODE_STANDALONE;
      ob_device->setMultiDeviceSyncConfig(ob_sync_config);
      orbbec_context().erase_from_software_sync_list(ob_device);
    }

    try {
      pc::logger()->trace("attempting to start pipeline");
      pipeline.start(ob_config);
    } catch (const ob::Error &e) {
      pc::logger()->error("Failed to start Orbbec pipeline: {}",
                          e.getMessage());
      set_error_state(true);
      return;
    }

    // auto ob_camera_parameters = pipeline.getCameraParam();
    auto ob_calibration_parameters = pipeline.getCalibrationParam(ob_config);

    backend::CameraIntrinsics color_intrinsics;
    size_t max_point_count{};

    if (sensor_config.conversion_mode.value() ==
        OrbbecDeviceConfiguration::PointConversionMode::D2C) {
      // we need the colour intrinsic to transform the depth point to
      // colour space
      const auto ob_color_intrinsics =
          ob_calibration_parameters.intrinsics[OB_SENSOR_COLOR];

      color_intrinsics = backend::util::make_camera_intrinsics(
          ob_color_intrinsics.fx, ob_color_intrinsics.fy,
          ob_color_intrinsics.cx, ob_color_intrinsics.cy, colour_width);

      max_point_count = colour_width * colour_height;

    } else if (sensor_config.conversion_mode.value() ==
               OrbbecDeviceConfiguration::PointConversionMode::C2D) {
    }

    ob_device_api_access.unlock();

    pc::logger()->trace("Starting process loop for OrbbecDevice {}",
                        device_config.id);

    // processing loop
    while (!stop_token.stop_requested()) {
      std::shared_ptr<ob::FrameSet> frame_set;
      try {
        frame_set = pipeline.waitForFrames(100);
      } catch (const ob::Error &e) {
        pc::logger()->error("waitForFrames error: {}", e.getMessage());
        continue;
      }
      if (!frame_set) continue;

      if (!in_any_session()) continue;

      ProfilingZone new_frame_zone("OrbbecDevice::new_frame");
      new_frame_zone.text(device_config.id);

      if (_loading_pipeline.load(std::memory_order_relaxed)) {
        set_loading(false);
      }

      // TODO find a way to cache this world transform position or somehow
      // otherwise remove the lock to get it
      pc::float4x4 world;
      {
        std::scoped_lock lock(_workspace->config_access);
        world = pc::devices::effective_world_transform(_workspace->config,
                                                       device_config.id);
      }

      // drop this frame rather than queue it when processing is behind...
      if (!try_begin_frame_task()) {
        continue;
      }

      // TODO evaluate if the hand off to an arbitrary pool is actually wise
      // like this. I think im increasing latency? idk

      pc::task_pool().detach_task(
          [this, &color_intrinsics, frame_set = std::move(frame_set),
           device_config = device_config, max_point_count = max_point_count,
           world = world]() {
            FrameTaskSlot frame_task_slot(*this);

            auto colour_frame = frame_set->colorFrame();
            auto depth_frame = frame_set->depthFrame();
            if (!colour_frame || !depth_frame) return;

            ProfilingZone process_frame_zone("OrbbecDevice::process_frame");

            const auto frame_width = colour_frame->width();
            const auto frame_height = colour_frame->height();
            const auto point_count =
                static_cast<size_t>(frame_width * frame_height);

            auto point_cloud = std::make_shared<PointCloud>();
            point_cloud->resize(max_point_count);

            const auto *ob_depth_frame_ptr =
                reinterpret_cast<const uint16_t *>(depth_frame->data());
            const auto *ob_color_frame_ptr =
                reinterpret_cast<const color_rgb *>(colour_frame->data());

            std::span ob_depth_data{ob_depth_frame_ptr, point_count};
            std::span ob_color_data{ob_color_frame_ptr, point_count};

            const auto backend_type = current_backend_type();
            const bool using_cuda = backend_type == BackendType::CUDA;
            backend::BackendPlugin *backend = backend_for(backend_type);

            // for CUDA, pre-allocate render buffer so it can be packed in the
            // same kernel pass as projection
            std::shared_ptr<std::vector<std::byte>> cuda_render_buffer;
            std::span<std::byte> render_output;
            if (using_cuda) {
              cuda_render_buffer = std::make_shared<std::vector<std::byte>>(
                  max_point_count * 16);
              render_output = *cuda_render_buffer;
            }

            {
              ProfilingZone backend_transform_zone(
                  "OrbbecDevice::backend_transform");
              if (backend) {
                backend->project_transform_frame_data(
                    ob_depth_data, ob_color_data, *point_cloud,
                    color_intrinsics, device_config.transform.value(),
                    device_config.color.value(), world, render_output);
              }
            }

            feed_operator_pipeline(std::move(point_cloud));

            if (auto *cpu = cpu_backend(); backend && cpu) {
              if (using_cuda && cuda_render_buffer) {
                _latest_render_data.store(std::move(cuda_render_buffer),
                                          std::memory_order_release);
              } else if (auto processed = _pipeline->latest_cloud()) {
                auto render_buffer = std::make_shared<std::vector<std::byte>>(
                    processed->size() * render_vertex_stride(*processed));
                cpu->pack_render_buffer(*processed, *render_buffer);
                _latest_render_data.store(std::move(render_buffer),
                                          std::memory_order_release);
              }
            }

            notify_point_cloud_updated();
            set_updated_time(steady_clock::now());
          });
    }

    // wait for detached processing tasks to finish before cleanup
    while (_process_tasks_in_flight.load() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

  } catch (const ob::Error &e) {
    // sdk v1's ob::Error is not a std::exception, catch it explicitly...
    pc::logger()->error("Exception in Orbbec processing thread ({}): {}",
                        e.getName(), e.getMessage());
    set_error_state(true);
  } catch (const std::exception &e) {
    pc::logger()->error("Exception in Orbbec processing thread: {}", e.what());
    set_error_state(true);
  } catch (...) {
    pc::logger()->error("Unknown exception in Orbbec processing thread");
    set_error_state(true);
  }
}

void OrbbecDevice::lidar_pipeline_thread_work(
    [[maybe_unused]] std::stop_token stop_token,
    [[maybe_unused]] std::shared_ptr<ob::Device> ob_device) {
#if POINTCASTER_ORBBEC_SDK_VERSION < 2
  pc::logger()->error("Lidar sensors are not compatible with Orbbec SDK v1. "
                      "Change the SDK version in Preferences > Orbbec",
                      POINTCASTER_ORBBEC_SDK_VERSION);
#else
  try {

    // TODO drive the lidar sensor stream

    // 1. initialise and start pipeline

    // we get exclusive access to the orbbec device api during initialisation
    // of this thread, but unlock before starting the working loop. this
    // ensures that devices are loaded serially because the sdk is not thread
    // safe
    std::unique_lock<std::mutex> ob_device_api_access(
        orbbec_context().device_api_access);

    ob::Pipeline pipeline{ob_device};

    auto ob_config = std::make_shared<ob::Config>();

    auto &device_config = std::get<OrbbecDeviceConfiguration>(_config);
    auto &sensor_config =
        rfl::get<OrbbecDeviceConfiguration::LidarSensorConfiguration>(
            device_config.sensor.get().variant());

    std::shared_ptr<ob::Sensor> lidar_sensor = nullptr;

    const auto sensor_list = ob_device->getSensorList();
    for (size_t i = 0; i < sensor_list->getCount(); i++) {
      const auto sensor_type = sensor_list->getSensorType(i);
      const auto sensor_type_str =
          ob::TypeHelper::convertOBSensorTypeToString(sensor_type);
      if (sensor_type_str == "LiDAR") {
        lidar_sensor = sensor_list->getSensor(i);
        break;
      }
    }

    // the only scan rates available to the pulsar sl450
    static const std::map<OBLiDARScanRate, uint32_t> available_scan_rates = {
        {OB_LIDAR_SCAN_15HZ, 15},
        {OB_LIDAR_SCAN_20HZ, 20},
        {OB_LIDAR_SCAN_25HZ, 25},
        {OB_LIDAR_SCAN_30HZ, 30},
        {OB_LIDAR_SCAN_40HZ, 40}};

    std::shared_ptr<ob::StreamProfile> target_profile = nullptr;

    auto stream_profiles = lidar_sensor->getStreamProfileList();
    for (size_t i = 0; i < stream_profiles->getCount(); i++) {
      const auto profile =
          stream_profiles->getProfile(i)->as<ob::LiDARStreamProfile>();
      auto it = available_scan_rates.find(profile->getScanRate());
      if (it != available_scan_rates.end()) {
        if (it->second == sensor_config.scan_rate.value()) {
          target_profile = profile;
          pc::logger()->trace("Initialising sensor with scan rate of {} hz",
                              sensor_config.scan_rate.value());
          break;
        }
      }
    }

    ob_config->enableStream(target_profile);
    ob_config->setFrameAggregateOutputMode(
        OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE);

    try {
      pc::logger()->trace("attempting to start pipeline");
      pipeline.start(ob_config);
    } catch (const ob::Error &e) {
      pc::logger()->error("Failed to start Orbbec pipeline: {}",
                          e.getMessage());
      set_error_state(true);
      return;
    }

    //
    // // Get property
    // ob_device->getStructuredData(OB_RAW_DATA_LIDAR_IP_ADDRESS, data,
    // &dataSize);
    // // Set property
    // ob_device->setIntProperty(OB_PROP_LIDAR_TAIL_FILTER_LEVEL_INT, 0);
    //

    ob_device_api_access.unlock();

    // scan samples are polar: an angle in degrees and a distance in mm.
    // no return at that angle
    std::optional<pc::float4x4> last_world;

    while (!stop_token.stop_requested()) {
      std::shared_ptr<ob::FrameSet> frame_set;
      try {
        frame_set = pipeline.waitForFrames(100);
      } catch (const ob::Error &e) {
        pc::logger()->error("waitForFrames error: {}", e.getMessage());
        continue;
      }
      if (!frame_set) continue;

      if (!in_any_session()) continue;

      ProfilingZone new_frame_zone("OrbbecDevice::new_frame");
      new_frame_zone.text(device_config.id);

      if (_loading_pipeline.load(std::memory_order_relaxed)) {
        set_loading(false);
      }

      // TODO find a way to cache this world transform position or somehow
      // otherwise remove the lock to get it
      pc::float4x4 world;
      {
        std::scoped_lock lock(_workspace->config_access);
        world = pc::devices::effective_world_transform(_workspace->config,
                                                       device_config.id);
        if (last_world != world) {
          last_world = world;
          pc::logger()->debug(
              "orbbec '{}' ancestor transform -> address='{}' offset=({}, "
              "{}, "
              "{})mm {}",
              device_config.id,
              pc::devices::device_address(_workspace->config, device_config.id),
              world.values[3], world.values[7], world.values[11],
              world == pc::float4x4{} ? "(identity, not applied)" : "");
        }
      }

      // drop this frame rather than queue it when processing is behind...
      if (!try_begin_frame_task()) {
        continue;
      }

      const auto &process_frame_task = [this, frame_set = std::move(frame_set),
                                        device_config,
                                        world = std::move(world)] {
        // frame_task_slot occupies a frame processing thread for its
        // lifetime... it's what tells try_begin_frame_task() to fail if too
        // many exist per device
        // TODO how many? where does the concurrent frame processing count
        // come from?
        const auto &sensor_config =
            rfl::get<OrbbecDeviceConfiguration::LidarSensorConfiguration>(
                device_config.sensor.get().variant());

        ProfilingZone process_frame_zone("OrbbecDevice::process_lidar_frame");
        FrameTaskSlot frame_task_slot(*this);

        auto frame = frame_set->getFrame(OB_FRAME_LIDAR_POINTS);
        auto lidar_frame = frame->as<ob::LiDARPointsFrame>();

        std::span raw_scan_samples{
            reinterpret_cast<const OBLiDARScanPoint *>(lidar_frame->getData()),
            lidar_frame->getDataSize() / sizeof(OBLiDARScanPoint)};

        constexpr float minimum_distance = 1.0f;
        constexpr float degrees_to_radians = std::numbers::pi_v<float> / 180.0f;

        // constexpr uint16_t minimum_intensity = 1;
        constexpr uint16_t maximum_intensity = 2000;

        using ColorMapping = OrbbecDeviceConfiguration::ColorMapping;
        const auto color_mapping = sensor_config.color_mapping.value();
        // cuts the scan off, and doubles as the range the Depth mapping
        const auto &distance_cap = sensor_config.maximum_distance.value();
        const auto maximum_distance =
            distance_cap.active
                ? std::max(static_cast<float>(distance_cap.value.mm), 1.0f)
                : static_cast<float>(std::numeric_limits<uint16_t>::max());

        auto filtered_cloud =
            raw_scan_samples | std::views::filter([&](const auto &scan_sample) {
              // here we can filter the cloud based on scan distance
              const auto &distance = scan_sample.distance;
              if (distance < minimum_distance || distance > maximum_distance) {
                return false;
              }
              // TODO and by minimum and maximum angles
              return true;
            }) |
            std::views::transform([&](const auto &scan_sample) {
              // then transform the incoming OBLidarScanPoint types into
              // world-space XYZ along with mapped colors
              const auto &distance = scan_sample.distance;
              const auto angle = scan_sample.angle * degrees_to_radians;
              const auto x = distance * std::sin(angle);
              const auto z = -distance * std::cos(angle);
              const position pos{.x = static_cast<int16_t>(std::lround(x)),
                                 .y = 0,
                                 .z = static_cast<int16_t>(std::lround(z))};

              const auto col = [&]() -> color {
                if (color_mapping == ColorMapping::Solid) {
                  return {255, 255, 255, 255};
                }
                if (color_mapping == ColorMapping::Depth) {
                  // near returns come out blue and far ones red
                  constexpr float blue_hue = 2.0f / 3.0f;
                  const float normalised_distance =
                      std::clamp(distance / maximum_distance, 0.0f, 1.0f);
                  return hue_color(blue_hue * (1.0f - normalised_distance));
                }
                const uint16_t scan_intensity =
                    std::min(scan_sample.intensity, maximum_intensity);
                const float normalised_intensity =
                    color_mapping == ColorMapping::LogIntensity
                        ? std::log1p(static_cast<float>(scan_intensity)) /
                              std::log1p(static_cast<float>(maximum_intensity))
                        : static_cast<float>(scan_intensity) /
                              maximum_intensity;
                const auto intensity = static_cast<unsigned char>(
                    std::lround(normalised_intensity * 255.0f));
                return {intensity, intensity, intensity, 255};
              }();

              return std::pair{pos, col};
            });

        auto scan_cloud = std::make_shared<PointCloud>();
        scan_cloud->reserve(raw_scan_samples.size());
        for (auto [pos, col] : filtered_cloud) {
          scan_cloud->positions.push_back(pos);
          scan_cloud->colors.push_back(col);
        }
        if (scan_cloud->empty()) return;

        backend::BackendPlugin *backend = current_backend();
        if (!backend) return;

        auto point_cloud = std::make_shared<PointCloud>();
        point_cloud->resize(scan_cloud->size());

        {
          ProfilingZone backend_transform_zone(
              "OrbbecDevice::backend_transform");
          backend->transform_point_cloud(*scan_cloud, *point_cloud,
                                         device_config.transform.value(),
                                         device_config.color.value(), world);
        }

        feed_operator_pipeline(std::move(point_cloud));

        if (auto *cpu = cpu_backend(); cpu) {
          if (auto processed = _pipeline->latest_cloud()) {
            auto render_buffer = std::make_shared<std::vector<std::byte>>(
                processed->size() * render_vertex_stride(*processed));
            cpu->pack_render_buffer(*processed, *render_buffer);
            _latest_render_data.store(std::move(render_buffer),
                                      std::memory_order_release);
          }
        }

        notify_point_cloud_updated();
        set_updated_time(steady_clock::now());
      };

      // TODO detach onto task pool?
      process_frame_task();
    }

  } catch (const std::exception &e) {
    pc::logger()->error("Exception in Orbbec processing thread: {}", e.what());
    set_error_state(true);
  } catch (...) {
    pc::logger()->error("Unknown exception in Orbbec processing thread");
    set_error_state(true);
  }
#endif
  set_error_state(true);
}

std::shared_ptr<PointCloud> OrbbecDevice::point_cloud() {
  if (_pipeline) {
    auto latest = _pipeline->latest_cloud();
    if (latest) return latest;
  }
  static auto empty = std::make_shared<PointCloud>();
  return empty;
}

void OrbbecDevice::timeout_thread_work(std::stop_token stop_token) {
  constexpr auto error_timeout = 10s;
  constexpr auto recovery_window = 500ms;
  constexpr auto check_interval = 1s;

  auto get_id = [this]() -> std::string {
    const auto &cfg = this->config_variant();
    if (auto *p = std::get_if<OrbbecDeviceConfiguration>(&cfg)) return p->id;
    return {};
  };

  std::mutex sleep_access;
  std::condition_variable_any sleep_wake;
  auto sleep_until_stopped = [&](auto duration) {
    std::unique_lock lock(sleep_access);
    sleep_wake.wait_for(lock, stop_token, duration,
                        [&] { return stop_token.stop_requested(); });
  };

  while (!stop_token.stop_requested()) {

    const auto now = steady_clock::now();
    const auto last_frame_time =
        _last_updated_time.load(std::memory_order_relaxed);

    const bool has_seen_a_frame = last_frame_time != steady_clock::time_point{};

    // we can update metrics on this thread too
    pc::metrics::set_gauge("pointcaster_orbbec_pipeline_hertz",
                           _pipeline_fps_ema.load(), {{"device_id", get_id()}});

    if (_running_pipeline && !_in_error_state) {
      if (!has_seen_a_frame) {
        sleep_until_stopped(check_interval);
        continue;
      }
      if (now - last_frame_time >= error_timeout) {
        set_error_state(true);
        pc::logger()->error(
            "Orbbec device '{}' entered error state. Attempting restart...",
            get_id());
        restart();
      }
    } else if (_in_error_state) {
      if (has_seen_a_frame && (now - last_frame_time <= recovery_window)) {
        set_error_state(false);
      }
    }
    sleep_until_stopped(check_interval);
  }
}

void OrbbecDevice::on_config_field_changed(std::string_view path) {
  auto &config = std::get<OrbbecDeviceConfiguration>(_config);
  auto &network_config = config.network.value();

  if (network_config.apply.value()) {
    network_config.apply.set(false);
    set_ip(network_config.ip_address.value(),
           network_config.subnet_mask.value(),
           network_config.gateway_address.value());
  }

  // a different sensor type needs a different pipeline
  if (path == "sensor") restart();
}

void OrbbecDevice::update_config(const DeviceConfigurationVariant &config) {
  DevicePlugin::update_config(config);
  if (!std::holds_alternative<OrbbecDeviceConfiguration>(_config)) return;

  // start the device when the first configuration is applied
  std::call_once(_kickoff_once, [this] {
    orbbec_context().run_on_ready([this] {
      notify_status_changed();
      start();
    });
  });
}

void OrbbecDevice::set_ip(std::string_view, std::string_view,
                          std::string_view) {
  // void OrbbecDevice::set_ip(std::string_view ip_address,
  //                           std::string_view subnet_mask,
  //                           std::string_view gateway_address) {
  // using pc::util::parse_ip_string;

  // auto ip_address_buffer = parse_ip_string(ip_address);
  // if (!ip_address_buffer.has_value()) {
  //   pc::logger()->error("Unable to update IP Address: {}",
  //                       ip_address_buffer.error());
  //   return;
  // }
  // auto subnet_mask_buffer = parse_ip_string(subnet_mask);
  // if (!subnet_mask_buffer.has_value()) {
  //   pc::logger()->error("Unable to update subnet mask: {}",
  //                       subnet_mask_buffer.error());
  //   return;
  // }
  // auto gateway_address_buffer = parse_ip_string(gateway_address);
  // if (!subnet_mask_buffer.has_value()) {
  //   pc::logger()->error("Unable to update gateway address: {}",
  //                       gateway_address_buffer.error());
  //   return;
  // }

  // OBNetIpConfig net_config{};
  // net_config.dhcp = 0;
  // std::copy(ip_address_buffer->begin(), ip_address_buffer->end(),
  //           net_config.address);
  // std::copy(subnet_mask_buffer->begin(), subnet_mask_buffer->end(),
  //           net_config.mask);
  // std::copy(gateway_address_buffer->begin(), gateway_address_buffer->end(),
  //           net_config.gateway);

  // auto ob_ctx = orbbec_context().wait_til_ready();
  // auto &config = std::get<OrbbecDeviceConfiguration>(_config);
  // pc::logger()->trace("Forcing IP...");
  // auto set_result = ob_ctx->forceIp(config.id.c_str(), net_config);
  // if (!set_result) {
  //   pc::logger()->error("Failed to set network configuration");
  // }
  // pc::logger()->info(
  //     "Successfully updated network config for OrbbecDevice '{}'",
  //     config.id);
}

} // namespace pc::devices

CORRADE_PLUGIN_REGISTER(OrbbecDevice, pc::devices::OrbbecDevice,
                        "net.pointcaster.DevicePlugin/1.0")
