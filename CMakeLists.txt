cmake_minimum_required(VERSION 4.4)

option(WITH_POINTCASTER "Build the pointcaster application" ON)
option(WITH_RECEIVER_LIB "Build the pointreceiver client library" OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(with_tracy_default OFF)
else()
    set(with_tracy_default ON)
endif()
option(WITH_TRACY "Build with the Tracy profiler client" ${with_tracy_default})

# we can use cmake to set which vcpkg dependencies we need
set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
if(WITH_POINTCASTER)
    list(APPEND VCPKG_MANIFEST_FEATURES "pointcaster")
endif()
if(WITH_RECEIVER_LIB)
    list(APPEND VCPKG_MANIFEST_FEATURES "pointreceiver")
endif()

project(pointcaster VERSION 0.2.15 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # heavily templated TUs (reflectcpp, pcl, boost) need this
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(WITH_POINTCASTER)
    include(Qt)
    include(QmlAdapterGenerator)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_custom_target(link_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json)

# flatten windows installs: exe and dlls at the install root with plugins/ and qml/ beside them
if(WIN32)
    set(CMAKE_INSTALL_BINDIR ".")
endif()

add_subdirectory(src)

if(WITH_POINTCASTER)
    include(Packaging)
endif()
