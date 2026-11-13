# divaft_Android — shader_recompiler 交叉编译（阶段3：GCN→IR→SPIR-V，shaders→Vulkan 核心）
# 首版聚焦编译：shader_recompiler 全部 + common/video_core 依赖 + sirit(SPIR-V)/fmt。
# 由 CI 用 NDK Clang(arm64-v8a, C++23) 构建；缺失依赖按 CI 报错迭代补齐。
cmake_minimum_required(VERSION 3.24)
project(divaft_shader_recompiler CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SHADPS4 "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/shadps4" CACHE PATH "shadPS4 source")
if(NOT EXISTS "${SHADPS4}/src/shader_recompiler/recompiler.cpp")
  message(FATAL_ERROR "shadPS4 source not found at ${SHADPS4} (run fetch-shadps4.sh)")
endif()

# ---- externals ----
set(SIRIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHADPS4}/externals/sirit" "${CMAKE_BINARY_DIR}/sirit" EXCLUDE_FROM_ALL)
add_subdirectory("${SHADPS4}/externals/fmt" "${CMAKE_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)

# ---- shader_recompiler 源 ----
file(GLOB_RECURSE SHADER_SRCS CONFIGURE_DEPENDS "${SHADPS4}/src/shader_recompiler/*.cpp")

# ---- common / video_core 依赖（shader_recompiler 引用面）----
set(DEP_SRCS
  "${SHADPS4}/src/common/assert.cpp"
  "${SHADPS4}/src/common/io_file.cpp"
  "${SHADPS4}/src/common/path_util.cpp"
  "${SHADPS4}/src/common/string_util.cpp"
  "${SHADPS4}/src/common/native_clock.cpp"
  "${SHADPS4}/src/common/rdtsc.cpp"
  "${SHADPS4}/src/common/error.cpp"
  "${SHADPS4}/src/common/logging/log.cpp"
  "${SHADPS4}/src/common/logging/classes.cpp"
  "${SHADPS4}/src/video_core/amdgpu/pixel_format.cpp"
  "${SHADPS4}/src/video_core/amdgpu/regs.cpp"
)

add_library(divaft_shader_recompiler STATIC ${SHADER_SRCS} ${DEP_SRCS})
target_include_directories(divaft_shader_recompiler PRIVATE
  "${SHADPS4}"
  "${SHADPS4}/src"
  "${SHADPS4}/externals/sirit/include"
  "${SHADPS4}/externals/fmt/include"
  "${SHADPS4}/externals/magic_enum/include"
  "${SHADPS4}/externals/half/include"
  "${SHADPS4}/externals/robin-map/include"
  "${SHADPS4}/externals/ext-boost"
  "${SHADPS4}/externals/gcn/include"
  "${SHADPS4}/externals/vulkan-headers/include"
  "${SHADPS4}/externals/spirv-headers/include"
  "${SHADPS4}/externals/xxHash"
  "${SHADPS4}/externals/spdlog/include"
)
target_link_libraries(divaft_shader_recompiler PRIVATE sirit fmt::fmt)
target_compile_options(divaft_shader_recompiler PRIVATE -Wno-everything)
message(STATUS "shader_recompiler sources: ${SHADER_SRCS}")