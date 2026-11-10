#include "profiler.h"

#include <atomic>
#include <core/logger/logger.h>
#include <cstdlib>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

namespace pc::profiling {

#ifdef TRACY_ENABLE

namespace {
std::atomic<bool> profiler_started{false};
}

void start_profiler(bool system_tracing) {
  pc::logger()->trace("Starting Tracy profiling session");

  // system tracing can be expensive when threads exit,
  // so users might want general tracing but no sys_tracing...
  // its configurable via env vars so we set this here
  const auto *no_system_tracing = system_tracing ? "0" : "1";
#ifdef _WIN32
  _putenv_s("TRACY_NO_SYS_TRACE", no_system_tracing);
#else
  setenv("TRACY_NO_SYS_TRACE", no_system_tracing, 1);
#endif
  pc::logger()->info("Tracy system tracing {}",
                     system_tracing ? "enabled" : "disabled");

  // TracySetProgramName("pointcaster");
  tracy::StartupProfiler();
  profiler_started.store(true, std::memory_order_release);
  pc::logger()->info("Started Tracy profiling session");
}

bool profiler_running() noexcept {
  return profiler_started.load(std::memory_order_acquire);
}

#else
void start_profiler(bool) {}
bool profiler_running() noexcept {
  return false;
}
#endif

} // namespace pc::profiling