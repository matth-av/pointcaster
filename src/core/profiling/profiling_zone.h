#pragma once
#include <pointcaster/core.h>
#include <source_location>
#include <string_view>

namespace pc::profiling {

class POINTCASTER_CORE_EXPORT ProfilingZone {
public:
  explicit ProfilingZone(
      std::string_view name,
      std::source_location location = std::source_location::current());
  ~ProfilingZone();

  ProfilingZone(const ProfilingZone &) = delete;
  ProfilingZone &operator=(const ProfilingZone &) = delete;
  ProfilingZone(ProfilingZone &&) = delete;
  ProfilingZone &operator=(ProfilingZone &&) = delete;

  void text(std::string_view text);

  // manually close the profiling zone...
  // this is called automatically on destruction
  void end();

private:
  void *zone_data;
};

} // namespace pc::profiling