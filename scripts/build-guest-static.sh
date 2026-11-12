#!/usr/bin/env bash
# divaft_Android — 把 FEX guest-harness 静态链接，供 qemu-aarch64 用户态运行。
# Android 动态版依赖 /system/bin/linker64，qemu 无法加载；静态版无解释器依赖。
# Bionic 静态可执行要求 PT_TLS 对齐>=64：harness 各 TU 用 -fno-emulated-tls 重编。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${FEX_BUILD:-"$ROOT/build/fex-android"}
: "${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}"
cd "$BUILD"

# 重编 divaft 的两个 harness TU（native TLS）
for src in "$ROOT/runtime/guest_bridge.cpp" "$ROOT/runtime/guest_test.cpp"; do
  OBJ_REL="CMakeFiles/fexcore-guest-harness.dir${src}.o"
  CMD=$(ninja -t commands "$OBJ_REL" | grep 'clang++' | head -1)
  [[ -n "$CMD" ]] || { echo "no compile cmd for $src" >&2; exit 1; }
  eval "$CMD -fno-emulated-tls"
done

# 静态重链
LINK_CMD=$(ninja -t commands fexcore-guest-harness | tail -1)
STATIC_CMD=${LINK_CMD/-o Bin\/fexcore-guest-harness/-o Bin\/fexcore-guest-harness-static -static}
eval "$STATIC_CMD"
echo "STATIC_GUEST_OK=$BUILD/Bin/fexcore-guest-harness-static"
"$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -l Bin/fexcore-guest-harness-static | grep -i interp || echo "no interpreter (good, static)"
