add_library(pointcaster_plugins SHARED
    plugin_loader.cc
    devices/device_plugin.cc
    devices/device_tree.cc
)
add_library(pointcaster::plugins ALIAS pointcaster_plugins)

# to allow export of lib functions for use in runtime-loaded plugins
target_compile_definitions(pointcaster_plugins PRIVATE POINTCASTER_BUILD)

# TODO ??
find_package(TBB REQUIRED)

target_link_libraries(pointcaster_plugins PRIVATE
    pointcaster::core
    pointcaster::config
    pointcaster::pipeline
    pointcaster::app_settings
    Corrade::PluginManager
    TBB::tbb
)

target_include_directories(pointcaster_plugins PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

install(TARGETS pointcaster_plugins
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib EXCLUDE_FROM_ALL
)

find_package(cpplocate CONFIG REQUIRED)
target_link_libraries(pointcaster_plugins PRIVATE cpplocate::cpplocate)

# subdirectories are corrade plugins

set(DEVICES_PLUGIN_DIR "plugins/devices")
set(BACKEND_PLUGIN_DIR "plugins/backend")
set(OPERATORS_PLUGIN_DIR "plugins/operators")

# - static plugins
add_subdirectory(backend/cpu)

add_subdirectory(devices/null)
add_subdirectory(devices/ply)

# - dynamic plugins
add_subdirectory(backend/cuda)

add_subdirectory(devices/orbbec)

add_subdirectory(operators/fringe_removal)
add_subdirectory(operators/cluster_extraction)
add_subdirectory(operators/range_filter)
add_subdirectory(operators/chroma_key)
add_subdirectory(operators/noise)


# the $CORRADE_PLUGINS and $CORRADE_STATIC_PLUGINS lists should 
# be populated after adding plugin subdirectories

add_custom_target(plugins ALL DEPENDS ${CORRADE_PLUGINS})

target_link_libraries(pointcaster_plugins PRIVATE ${CORRADE_STATIC_PLUGINS})

target_precompile_headers(pointcaster_plugins PRIVATE
  <string>
  <string_view>
  <vector>
  <memory>
  <filesystem>
  <optional>
  <Corrade/PluginManager/Manager.h>
  <Corrade/PluginManager/AbstractManager.h>
  <Corrade/Containers/StringView.h>
)