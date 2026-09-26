# Android arm64-v8a for pointreceiver lib builds, release only
#
set(VCPKG_BUILD_TYPE release)

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_CMAKE_ANDROID_STL_TYPE c++_static)
set(CMAKE_ANDROID_STL_TYPE c++_static)
set(VCPKG_ANDROID_STL c++_static)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)

set(VCPKG_CXX_FLAGS_RELEASE "-O3")
set(VCPKG_C_FLAGS_RELEASE "-O3")
