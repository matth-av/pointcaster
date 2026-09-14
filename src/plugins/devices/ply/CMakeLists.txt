corrade_add_static_plugin(
  PlyDevice
  PlyDevice
  ""
  PlyDevice.conf
  ply_device.cc
  ply_layout.cc
  ply_sequence_loader.cc
  ${CMAKE_SOURCE_DIR}/src/ui/models/enum_adapters.h
)

set_target_properties(PlyDevice PROPERTIES AUTOMOC OFF)

find_package(TBB REQUIRED)
find_package(Boost REQUIRED COMPONENTS interprocess)
find_package(unofficial-miniply CONFIG REQUIRED)

target_link_libraries(PlyDevice PRIVATE
  pointcaster::core
  Corrade::PluginManager
  TBB::tbb
  Boost::interprocess
  unofficial::miniply::miniply
)

target_include_directories(PlyDevice
  PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
  ${TBB_INCLUDE_DIRS}
)

# remove the installed directory because its static, we don't need it
install(CODE [===[
  file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/PlyDevice/")
]===])

set(CORRADE_STATIC_PLUGINS ${CORRADE_STATIC_PLUGINS} PlyDevice PARENT_SCOPE)
set(CORRADE_PLUGINS ${CORRADE_PLUGINS} PlyDevice PARENT_SCOPE)
