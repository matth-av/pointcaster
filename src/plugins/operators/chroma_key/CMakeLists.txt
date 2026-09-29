set(CHROMA_KEY_PLUGIN_DIR ${OPERATORS_PLUGIN_DIR}/chroma_key)


enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 20)

find_package(CUDAToolkit REQUIRED COMPONENTS cudart)
find_package(Thrust REQUIRED CONFIG)
find_package(TBB REQUIRED)

if(NOT TARGET Thrust)
  thrust_create_target(Thrust DEVICE CUDA)
  target_link_libraries(Thrust INTERFACE CUDA::toolkit)
endif()

# corrade doesn't play nice with nvcc, so the cuda path is an 
# nvcc-compiled static library linked into the plugin
add_library(chroma_key_cuda STATIC
  chroma_key_cuda.cu
)

set_target_properties(chroma_key_cuda PROPERTIES
  AUTOMOC OFF
  CORRADE_USE_PEDANTIC_FLAGS OFF
  CUDA_SEPARABLE_COMPILATION ON
  CUDA_RESOLVE_DEVICE_SYMBOLS ON
  CUDA_PROPAGATE_HOST_FLAGS OFF
  CUDA_ARCHITECTURES "75-real;86-real;89-real;89-virtual"
)

if(WIN32)
  target_compile_options(chroma_key_cuda PRIVATE
    /bigobj /utf-8 -forward-slash-prefix-opts)
endif()

target_compile_options(chroma_key_cuda PRIVATE
  --extended-lambda
  --split-compile=0
  --threads=0
  # these turn off noisy compiler output from dependencies
  --diag-suppress 20012,20013,20014,20015,27,1394,221
)

target_link_libraries(chroma_key_cuda PUBLIC
  pointcaster::core
  CUDA::toolkit
  Thrust
)

target_include_directories(chroma_key_cuda PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
)

corrade_add_plugin(
  ChromaKeyOperator
  ${CHROMA_KEY_PLUGIN_DIR}
  ${CHROMA_KEY_PLUGIN_DIR}
  ChromaKeyOperator.conf
  chroma_key_operator.cc
  chroma_key_cpu.cc
)

set_target_properties(ChromaKeyOperator PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CHROMA_KEY_PLUGIN_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${CHROMA_KEY_PLUGIN_DIR}
  ARCHIVE_OUTPUT_DIRECTORY ${CHROMA_KEY_PLUGIN_DIR}
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/chroma_key"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/chroma_key"
  AUTOMOC OFF
)

target_link_libraries(ChromaKeyOperator PRIVATE
  pointcaster
  pointcaster::core
  pointcaster::pipeline
  Corrade::PluginManager
  chroma_key_cuda
  TBB::tbb
)

target_include_directories(ChromaKeyOperator PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
)

add_custom_command(TARGET ChromaKeyOperator POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROMA_KEY_PLUGIN_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/ChromaKeyOperator.conf"
  "${CHROMA_KEY_PLUGIN_DIR}/ChromaKeyOperator.conf"
)

target_precompile_headers(ChromaKeyOperator PRIVATE
  <algorithm>
  <execution>
  <filesystem>
  <memory>
  <ranges>
  <span>
  <string>
  <string_view>
  <variant>
  <vector>
  <Corrade/Containers/StringView.h>
  <Corrade/PluginManager/AbstractPlugin.h>
  <Corrade/PluginManager/Manager.h>
  <oneapi/tbb/blocked_range.h>
  <oneapi/tbb/parallel_reduce.h>
  <rfl/Literal.hpp>
  <rfl/TaggedUnion.hpp>
  <spdlog/logger.h>
)

set(CORRADE_PLUGINS ${CORRADE_PLUGINS} ChromaKeyOperator PARENT_SCOPE)
