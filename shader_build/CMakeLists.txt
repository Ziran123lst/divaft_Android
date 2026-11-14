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
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHADPS4}/externals/zydis" "${CMAKE_BINARY_DIR}/zydis" EXCLUDE_FROM_ALL)
# spdlog/fmt 顺序对齐 shadPS4 externals/CMakeLists.txt：先 spdlog（fmt::fmt 尚不存在→
# SPDLOG_FMT_EXTERNAL 保持 OFF，spdlog 自带 fmt 并创建 fmt::fmt），随后仅在缺 fmt 时补 add_subdirectory。
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON)
if(TARGET fmt::fmt)
  set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${SHADPS4}/externals/spdlog" "${CMAKE_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL)
if(NOT TARGET fmt::fmt)
  add_subdirectory("${SHADPS4}/externals/fmt" "${CMAKE_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)
endif()

# ---- shader_recompiler 源 ----
file(GLOB_RECURSE SHADER_SRCS CONFIGURE_DEPENDS "${SHADPS4}/src/shader_recompiler/*.cpp")
# ARM64：原版 flatten_extended_userdata_pass 用 x86 xbyak JIT（#ifdef ARCH_X86_64），以兼容版替换。
list(FILTER SHADER_SRCS EXCLUDE REGEX "flatten_extended_userdata_pass\\.cpp$")

# ---- common / video_core 依赖（shader_recompiler 引用面）----
set(DEP_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/shader_assert.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/shader_stubs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/flatten_userdata_arm64.cpp"
  "${SHADPS4}/src/common/io_file.cpp"
  "${SHADPS4}/src/common/path_util.cpp"
  "${SHADPS4}/src/common/string_util.cpp"
  "${SHADPS4}/src/common/native_clock.cpp"
  "${SHADPS4}/src/common/rdtsc.cpp"
  "${SHADPS4}/src/common/error.cpp"
  "${SHADPS4}/src/common/decoder.cpp"
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
  "${SHADPS4}/externals/sirit/externals/SPIRV-Headers/include"
  "${SHADPS4}/externals/xxhash"
  "${SHADPS4}/externals/spdlog/include"
  "${SHADPS4}/externals/json/include"
  "${SHADPS4}/externals/xbyak"
)
target_link_libraries(divaft_shader_recompiler PRIVATE sirit fmt::fmt Zydis::Zydis spdlog::spdlog)
target_compile_options(divaft_shader_recompiler PRIVATE -Wno-everything)
message(STATUS "shader_recompiler sources: ${SHADER_SRCS}")

# 端到端验证：GCN shader → SPIR-V（shaders→Vulkan 核心管线实跑）
add_executable(shader_translate_test "${CMAKE_CURRENT_SOURCE_DIR}/translate_test.cpp")
target_link_libraries(shader_translate_test PRIVATE divaft_shader_recompiler spdlog::spdlog)
target_include_directories(shader_translate_test PRIVATE
  "${SHADPS4}" "${SHADPS4}/src"
  "${SHADPS4}/externals/sirit/include" "${SHADPS4}/externals/fmt/include"
  "${SHADPS4}/externals/magic_enum/include" "${SHADPS4}/externals/half/include"
  "${SHADPS4}/externals/robin-map/include" "${SHADPS4}/externals/ext-boost"
  "${SHADPS4}/externals/gcn/include" "${SHADPS4}/externals/vulkan-headers/include"
  "${SHADPS4}/externals/sirit/externals/SPIRV-Headers/include"
  "${SHADPS4}/externals/spdlog/include" "${SHADPS4}/externals/json/include"
  "${SHADPS4}/externals/xbyak"
)
target_compile_options(shader_translate_test PRIVATE -Wno-everything)
# spdlog 以编译库模式统一（spdlog::spdlog 传播 SPDLOG_COMPILED_LIB）；不再定义 SPDLOG_HEADER_ONLY，避免与库模式冲突导致链接缺符号。