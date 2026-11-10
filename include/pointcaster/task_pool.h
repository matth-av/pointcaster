#pragma once

#include <BS_thread_pool.hpp>
#include <cstddef>
#include <pointcaster/core.h>

namespace pc {

// process-wide pool for CPU-bound work
// device frame processing, sequence decoding etc....
POINTCASTER_CORE_EXPORT BS::priority_thread_pool &task_pool();

POINTCASTER_CORE_EXPORT void set_task_pool_size(std::size_t thread_count);

} // namespace pc
