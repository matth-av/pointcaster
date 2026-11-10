#pragma once

#include <pointcaster/core.h>

namespace pc::profiling {

// system tracing adds call stack sampling and thread scheduling events
POINTCASTER_CORE_EXPORT void start_profiler(bool system_tracing = false);

POINTCASTER_CORE_EXPORT bool profiler_running() noexcept;

} // namespace pc::profiling