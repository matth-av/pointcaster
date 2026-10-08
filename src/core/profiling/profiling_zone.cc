#include "profiling_zone.h"

#include "profiler.h"

#ifdef TRACY_ENABLE
// - we need to wrap tracy so that theres a single instance in a shared
// library that all modules of code have access to
// - and Tracy's C API is used to avoid template issues in .cu files... nvcc
// throws a spat otherwise
#include <cstring>
#include <tracy/TracyC.h>

#ifndef TRACY_CALLSTACK
#define TRACY_CALLSTACK 0
#endif

namespace pc::profiling {

struct InternalZone {
  TracyCZoneCtx ctx;
};

ProfilingZone::ProfilingZone(std::string_view name,
                             std::source_location location)
    : zone_data(nullptr) {
  if (!profiler_running()) return;
  // Allocate our internal zone object.
  auto *iz = new InternalZone;
  // Allocate a source location using the name and the caller's location.
  // ___tracy_alloc_srcloc_name takes: line, file, file length, function,
  // function length, name, name length, color.
  const char *file = location.file_name();
  const char *function = location.function_name();
  uint64_t srcloc = ___tracy_alloc_srcloc_name(
      location.line(), file, std::strlen(file), function,
      std::strlen(function), name.data(), name.size(), 0);
  // Begin the zone with callstack depth TRACY_CALLSTACK.
  iz->ctx =
      ___tracy_emit_zone_begin_alloc_callstack(srcloc, TRACY_CALLSTACK, 1);
  zone_data = iz;
}

ProfilingZone::~ProfilingZone() { end(); }

void ProfilingZone::end() {
  if (zone_data) {
    auto *iz = static_cast<InternalZone *>(zone_data);
    ___tracy_emit_zone_end(iz->ctx);
    delete iz;
    zone_data = nullptr;
  }
}

void ProfilingZone::text(std::string_view text) {
  if (zone_data) {
    auto *iz = static_cast<InternalZone *>(zone_data);
    ___tracy_emit_zone_text(iz->ctx, text.data(),
                            static_cast<unsigned int>(text.size()));
  }
}

} // namespace pc::profiling

#else // TRACY_ENABLE not defined

namespace pc::profiling {

ProfilingZone::ProfilingZone(std::string_view, std::source_location)
    : zone_data(nullptr) {}
ProfilingZone::~ProfilingZone() {}
void ProfilingZone::text(std::string_view) {}
void ProfilingZone::end() {}

} // namespace pc::profiling

#endif