# divaft_Android — 顶层构建
# 阶段1：本机可编译加载器验证工具 divaft_load（验证原创加载器）。
# Android/FEX/shadPS4 交叉编译在阶段2+ 接入（见 docs/architecture.md）。
cmake_minimum_required(VERSION 3.22)
project(divaft_Android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DIVAFT_BUILD_TOOLS "Build loader validation tools" ON)
option(DIVAFT_ANDROID "Cross-compile runtime for Android (needs NDK + FEX/shadPS4)" OFF)

# 原创移植运行时（不依赖 FEX/shadPS4 的部分：加载器/HLE接线/输入）
add_library(divaft_runtime STATIC
  runtime/diva_loader.cpp
  runtime/hle_wire.cpp
  runtime/input_pad.cpp
  runtime/audio_out.cpp
  runtime/guest_bridge.cpp
)
target_include_directories(divaft_runtime PUBLIC runtime/include)
target_compile_options(divaft_runtime PRIVATE -Wall -Wextra)

if(DIVAFT_BUILD_TOOLS)
  add_executable(divaft_load runtime/main.cpp)
  target_link_libraries(divaft_load PRIVATE divaft_runtime)
endif()

# Android 交叉编译 + FEX/shadPS4 集成（阶段2+）
if(DIVAFT_ANDROID)
  # add_subdirectory(third_party/fex)
  # add_subdirectory(third_party/shadps4)
  # target_link_libraries(divaft_runtime PRIVATE FEXCore shadps4_video_core)
  message(STATUS "DIVAFT_ANDROID: FEX/shadPS4 integration pending (phase 2+)")
endif()
