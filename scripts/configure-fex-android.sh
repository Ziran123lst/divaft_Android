#!/usr/bin/env bash
# divaft_Android — 配置并构建 FEXCore（Android/ARM64，NDK/Bionic）
# 产出 libFEXCore.a + Common/CommonTools/Jemalloc，供 divaft_runtime 链接。
# 前置：scripts/fetch-fex.sh 已检出 FEX；ANDROID_NDK_ROOT 指向 NDK r30。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
FEX_SRC=${FEX_SRC:-"$ROOT/third_party/fex"}
BUILD=${FEX_BUILD:-"$ROOT/build/fex-android"}
: "${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT to NDK r30}"
[[ -d "$FEX_SRC/.git" ]] || { echo "FEX source missing (run fetch-fex.sh)" >&2; exit 1; }

# bionic 构建适配补丁（幂等：逆向再应用）
for P in "$ROOT/scripts/fex/fex-0001-android.patch" "$ROOT/scripts/fex/fex-0002-pause-wfe.patch"; do
  if git -C "$FEX_SRC" apply -R --check "$P" >/dev/null 2>&1; then
    git -C "$FEX_SRC" apply -R "$P"
  fi
  git -C "$FEX_SRC" apply "$P"
done

mkdir -p "$BUILD"
# 用 FEX 的 guest-harness 目标编译 divaft 的 guest_bridge + guest_test（自动链接 FEXCore）。
# DIVAFT_WITH_FEX 开启真实 FEX 集成；divaft 头文件路径经 FEXCORE_PROJECT_SOURCE_DIR 注入。
cmake -S "$FEX_SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-DDIVAFT_WITH_FEX" \
  -DTUNE_CPU=cortex-a78 \
  -DBUILD_FEXCORE_ONLY=ON \
  -DFEXCORE_SMOKE_SOURCE="$ROOT/scripts/fex/smoke_stub.cpp" \
  -DFEXCORE_GUEST_HARNESS_SOURCES="$ROOT/runtime/guest_bridge.cpp;$ROOT/runtime/diva_loader.cpp;$ROOT/runtime/hle_wire.cpp;$ROOT/runtime/diva_runtime.cpp;$ROOT/runtime/diva_boot.cpp" \
  -DFEXCORE_PROJECT_SOURCE_DIR="$ROOT/runtime/include" \
  -DBUILD_FEXCONFIG=OFF -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF \
  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_LTO=OFF \
  -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_CCACHE=OFF \
  -DENABLE_VIXL_DISASSEMBLER=OFF -DENABLE_VIXL_SIMULATOR=OFF \
  -DENABLE_ZYDIS=OFF -DENABLE_FEXCORE_PROFILER=OFF -DENABLE_WERROR=OFF
cmake --build "$BUILD" --target FEXCore fexcore-guest-harness -j4
echo "FEX_BUILD_OK $BUILD"
ls -la "$BUILD/Bin/fexcore-guest-harness"
