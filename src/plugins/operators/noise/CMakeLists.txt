set(NOISE_PLUGIN_DIR ${OPERATORS_PLUGIN_DIR}/noise)


enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 20)

find_package(CUDAToolkit REQUIRED COMPONENTS cudart)
find_package(Thrust REQUIRED CONFIG)
find_package(TBB REQUIRED)
find_package(glm CONFIG REQUIRED)

if(NOT TARGET Thrust)
  thrust_create_target(Thrust DEVICE CUDA)
  target_link_libraries(Thrust INTERFACE CUDA::toolkit)
endif()

# corrade doesn't play nice with nvcc, so the cuda path is an
# nvcc-compiled static library linked into the plugin
add_library(noise_cuda STATIC
  noise_cuda.cu
)

set_target_properties(noise_cuda PROPERTIES
  AUTOMOC OFF
  CORRADE_USE_PEDANTIC_FLAGS OFF
  CUDA_SEPARABLE_COMPILATION ON
  CUDA_RESOLVE_DEVICE_SYMBOLS ON
  CUDA_PROPAGATE_HOST_FLAGS OFF
  CUDA_ARCHITECTURES "75-real;86-real;89-real;89-virtual"
)

if(WIN32)
  target_compile_options(noise_cuda PRIVATE
    /bigobj /utf-8 -forward-slash-prefix-opts)
endif()

target_compile_options(noise_cuda PRIVATE
  --extended-lambda
  # required for glm's vector arithmetic
  --expt-relaxed-constexpr
  --split-compile=0
  --threads=0
  # these turn off noisy compiler output from deps
  --diag-suppress 20012,20013,20014,20015,27,1394,221
)

target_link_libraries(noise_cuda PUBLIC
  pointcaster::core
  CUDA::toolkit
  Thrust
  glm::glm-header-only
)

# pinning the language keeps
# the cpu and cuda paths on the same glm code on each platform
target_compile_definitions(noise_cuda PUBLIC GLM_FORCE_CXX20)

target_include_directories(noise_cuda PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
)

corrade_add_plugin(
  NoiseOperator
  ${NOISE_PLUGIN_DIR}
  ${NOISE_PLUGIN_DIR}
  NoiseOperator.conf
  noise_operator.cc
  noise_cpu.cc
)

set_target_properties(NoiseOperator PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${NOISE_PLUGIN_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${NOISE_PLUGIN_DIR}
  ARCHIVE_OUTPUT_DIRECTORY ${NOISE_PLUGIN_DIR}
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/noise"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/noise"
  AUTOMOC OFF
)

target_link_libraries(NoiseOperator PRIVATE
  pointcaster
  pointcaster::core
  pointcaster::pipeline
  Corrade::PluginManager
  noise_cuda
  TBB::tbb
  glm::glm-header-only
)

target_include_directories(NoiseOperator PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include"
)

# these are required for predictable noise gradients between cpu and gpu
target_compile_options(NoiseOperator PRIVATE
  $<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-fno-reciprocal-math>
)

add_custom_command(TARGET NoiseOperator POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${NOISE_PLUGIN_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/NoiseOperator.conf"
  "${NOISE_PLUGIN_DIR}/NoiseOperator.conf"
)

target_precompile_headers(NoiseOperator PRIVATE
  <algorithm>
  <memory>
  <random>
  <string>
  <string_view>
  <variant>
  <vector>
  <Corrade/Containers/StringView.h>
  <Corrade/PluginManager/AbstractPlugin.h>
  <Corrade/PluginManager/Manager.h>
  <glm/gtc/noise.hpp>
  <oneapi/tbb/blocked_range.h>
  <oneapi/tbb/parallel_reduce.h>
  <rfl/Literal.hpp>
  <rfl/TaggedUnion.hpp>
  <spdlog/logger.h>
)

set(CORRADE_PLUGINS ${CORRADE_PLUGINS} NoiseOperator PARENT_SCOPE)
