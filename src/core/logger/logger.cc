#include "logger.h"
#include <deque>
#include <iterator>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#if defined(__ANDROID__)
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace pc::log::detail {

// extends spdlog sink to keep recent entries for the UI console to take
class buffer_sink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  static constexpr std::size_t max_entries = 1024;

  std::vector<pc::LogEntry> take() {
    // held for the swap only, so logging threads never wait on the consumer
    std::deque<pc::LogEntry> taken;
    {
      std::scoped_lock lock(base_sink::mutex_);
      taken.swap(_entries);
    }
    return std::vector<pc::LogEntry>(std::make_move_iterator(taken.begin()),
                                     std::make_move_iterator(taken.end()));
  }

  void set_wakeup(pc::LogWakeup wakeup) {
    std::scoped_lock lock(base_sink::mutex_);
    _wakeup = std::move(wakeup);
  }

protected:
  void sink_it_(const spdlog::details::log_msg &msg) override {
    const bool was_empty = _entries.empty();

    _entries.push_back(pc::LogEntry{
        msg.level, std::string(msg.payload.data(), msg.payload.size()),
        msg.time});
    if (_entries.size() > max_entries) _entries.pop_front();

    // the rest of a burst is picked up by the same take
    if (was_empty && _wakeup) _wakeup();
  }

  void flush_() override {}

private:
  std::deque<pc::LogEntry> _entries;
  pc::LogWakeup _wakeup;
};

static std::shared_ptr<spdlog::logger> instance;
static std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_sink;
static std::shared_ptr<buffer_sink> buffer_sink_instance;

static std::shared_ptr<spdlog::logger> &get_logger_impl() {
  if (!instance) {
    buffer_sink_instance = std::make_shared<buffer_sink>();
#if defined(__ANDROID__)
    auto console_sink =
        std::make_shared<spdlog::sinks::android_sink_mt>("pointreceiver", true);
#else
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
#endif
    std::vector<spdlog::sink_ptr> sinks{buffer_sink_instance, console_sink};
    instance =
        std::make_shared<spdlog::logger>("pc", sinks.begin(), sinks.end());
    instance->set_level(spdlog::level::info);
  }
  return instance;
}

static std::filesystem::path log_directory_for(std::string_view target_name) {
  const auto get_env_path =
      [](const char *name) -> std::optional<std::filesystem::path> {
    if (!name) return std::nullopt;
    const char *v = std::getenv(name);
    if (!v || *v == '\0') return std::nullopt;
    return std::filesystem::path(v);
  };

#if defined(_WIN32)
  // %LOCALAPPDATA%\matth\<app>
  if (auto p = get_env_path("LOCALAPPDATA"))
    return (*p) / "matth" / std::string(target_name);
  if (auto p = get_env_path("APPDATA"))
    return (*p) / "matth" / std::string(target_name);
  return std::filesystem::current_path() / "Log" / std::string(target_name);
#else
  // $XDG_STATE_HOME/matth/<app>
  // else ~/.local/state/matth/<app>
  if (auto p = get_env_path("XDG_STATE_HOME"))
    return (*p) / "matth" / std::string(target_name);
  if (auto home = get_env_path("HOME"))
    return (*home) / ".local" / "state" / "matth" / std::string(target_name);
  return std::filesystem::current_path() / "matth" / std::string(target_name);
#endif
}

static std::filesystem::path log_file_path(std::string_view target_name) {
  const auto dir = log_directory_for(target_name);
  return dir / (std::string(target_name) + ".log");
}

static void rotate_file_logs(const std::filesystem::path &base_file,
                             std::size_t keep_count) {
  if (keep_count < 2) {
    std::error_code ec_rm;
    std::filesystem::remove(base_file, ec_rm);
    return;
  }

  const auto make_rotated = [&](std::size_t idx) -> std::filesystem::path {
    return base_file.string() + "." + std::to_string(idx);
  };

  std::filesystem::remove(make_rotated(keep_count - 1));

  for (std::size_t i = keep_count - 1; i-- > 1;) {
    const auto src = make_rotated(i);
    const auto dst = make_rotated(i + 1);
    if (std::filesystem::exists(src)) std::filesystem::rename(src, dst);
  }

  if (std::filesystem::exists(base_file))
    std::filesystem::rename(base_file, make_rotated(1));
}

} // namespace pc::log::detail

namespace pc {

std::shared_ptr<spdlog::logger> &logger() {
  return pc::log::detail::get_logger_impl();
}

void set_log_level(spdlog::level::level_enum lvl) {
  logger()->set_level(lvl);
}

void enable_file_logging(std::string_view target) {
  auto &logger = pc::logger();

  constexpr std::size_t keep_runs = 5;

  assert(!target.empty());
  const auto path = pc::log::detail::log_file_path(target);

  // If already enabled to same file, no-op.
  if (pc::log::detail::file_sink) {
    try {
      const auto current_filename = pc::log::detail::file_sink->filename();
      if (std::filesystem::path(current_filename) == path) return;
    } catch (...) {
    }

    auto &sinks = logger->sinks();
    sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                               [](const spdlog::sink_ptr &s) {
                                 return s == pc::log::detail::file_sink;
                               }),
                sinks.end());
    pc::log::detail::file_sink.reset();
  }

  // Ensure directory exists.
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  // TODO errors?

  pc::log::detail::rotate_file_logs(path, keep_runs);

  try {
    constexpr bool truncate = true;

    pc::log::detail::file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(),
                                                            truncate);

    pc::log::detail::file_sink->set_level(spdlog::level::trace);
    // pc::log::detail::file_sink->set_pattern(
    //     "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    logger->sinks().push_back(pc::log::detail::file_sink);

    logger->flush_on(spdlog::level::trace);

    logger->info("Logging output to '{}'", path.string());
  } catch (const std::exception &e) {
    logger->error("Failed to enable file logging to '{}': {}", path.string(),
                  e.what());
  } catch (...) {
    logger->error("Failed to enable file logging to '{}': <unknown error>",
                  path.string());
  }
}

void disable_file_logging() {
  if (!pc::log::detail::file_sink) return;

  auto &logger = pc::logger();

  auto &sinks = logger->sinks();
  sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                             [](const spdlog::sink_ptr &s) {
                               return s == pc::log::detail::file_sink;
                             }),
              sinks.end());

  pc::log::detail::file_sink.reset();
  logger->info("disabled file logging");
}

std::optional<std::filesystem::path> log_directory() {
  if (!pc::log::detail::file_sink) return std::nullopt;
  try {
    return std::filesystem::path(pc::log::detail::file_sink->filename())
        .parent_path();
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<LogEntry> take_log_entries() {
  (void)pc::logger();
  return pc::log::detail::buffer_sink_instance->take();
}

void set_log_wakeup(LogWakeup wakeup) {
  (void)pc::logger();
  pc::log::detail::buffer_sink_instance->set_wakeup(std::move(wakeup));
}

} // namespace pc
