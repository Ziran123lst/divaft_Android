#!/bin/bash
# DIVA 全 GPU 模式 qemu 自主验证：Xvfb + SDL x11 + lavapipe 软件 Vulkan。
# 前置：fetch_sysroot.sh + diva_glibc_deps.sh + diva_arm64_build.sh 已跑；
#       boot-linked.bin/libbbaot.so(glibc)/content.bin 在 $TEST 目录。
set -euo pipefail
TEST=${DIVA_TEST_DIR:-/root/diva-test}
BA=${DIVA_BB_ANDROID:-/root/divaft-work/bb-android}
SR=$BA/.local-deps/android/arm64-sysroot
GAME=${1:?用法: run_qemu_gpu.sh <游戏目录CUSA08053> [超时秒]}
TMO=${2:-150}
mkdir -p "$TEST/logs" "$TEST/syslibs" "$TEST/vk"
cp "$BA/out/arm64/bin/bb-probe" "$BA/out/arm64/lib/"*.so "$TEST/" 2>/dev/null || true
for l in libm libc libdl libpthread librt libresolv libutil; do
  real=$(ls $SR/usr/lib/aarch64-linux-gnu/$l.so.[0-9] 2>/dev/null | head -1)
  [ -n "$real" ] && ln -sf "$real" "$TEST/syslibs/$l.so"
done
cat > "$TEST/vk/lvp.json" <<JSON
{"ICD":{"api_version":"1.4.305","library_path":"$SR/usr/lib/aarch64-linux-gnu/libvulkan_lvp.so"},"file_format_version":"1.0.0"}
JSON
mkdir -p "$HOME/.local/share/shadPS4"
ls /tmp/.X11-unix/X99 >/dev/null 2>&1 || { Xvfb :99 -screen 0 1920x1080x24 >/tmp/xvfb.log 2>&1 & sleep 2; }
cd "$TEST"
timeout $((TMO+130)) qemu-aarch64 -L "$SR" \
  -E LD_LIBRARY_PATH="$TEST/syslibs:$TEST:$SR/usr/lib/aarch64-linux-gnu" \
  -E BB_AOT="$TEST/libbbaot.so" -E BB_IMPORT_STUBS=1 \
  -E SDL_VIDEODRIVER=x11 -E DISPLAY=:99 \
  -E VK_ICD_FILENAMES="$TEST/vk/lvp.json" \
  ./bb-probe boot-linked.bin --app0 "$GAME" --content-profile content.bin --timeout "$TMO" 2>&1 | tee gpu-boot.log
