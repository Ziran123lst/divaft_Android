#!/bin/bash
# DIVA 版 bionic 运行时构建：libbbcpu + libbbgpu(BB_NET=OFF) + bb-probe
set -euo pipefail
cd /root/divaft-work/bb-android
out=out/bionic
root=$PWD/.local-deps/android
ndk=${BB_NDK:-/root/android-ndk-r30}
api=30
prefix=$root/bionic-prefix
headers=$PWD/.local-deps/root/usr
toolchain=$ndk/build/cmake/android.toolchain.cmake
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
J=4
ndk_cmake=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared)
for patch in gpu/patches/fsr-vulkan/*.patch; do
  if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
    git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch" || true
  fi
done
if [[ ! -f $out/cpu/build.ninja ]]; then
  cmake -S cpu -B "$out/cpu" -G Ninja "${ndk_cmake[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
fi
ninja -C "$out/cpu" -j $J bbcpu > "$out/cpu-build.log" 2>&1 || { grep -E 'error|FAILED' "$out/cpu-build.log" | head -30; exit 1; }
echo CPU_OK
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$prefix/share/pkgconfig
export PKG_CONFIG_LIBDIR=$PKG_CONFIG_PATH
if [[ ! -f $out/gpu/build.ninja ]]; then
  cmake -S gpu -B "$out/gpu" -G Ninja "${ndk_cmake[@]}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF -DBB_NET=OFF \
    "-DCMAKE_FIND_ROOT_PATH=$prefix;$headers" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH "-DCMAKE_PREFIX_PATH=$prefix;$headers" \
    -Dmagic_enum_DIR="$headers/share/cmake/magic_enum" \
    -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
    -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
    -DBoost_DIR="$headers/lib/cmake/Boost-1.92.0" \
    -Dboost_headers_DIR="$headers/lib/cmake/boost_headers-1.92.0" > "$out/gpu-cfg.log" 2>&1 || { tail -20 "$out/gpu-cfg.log"; exit 1; }
fi
cmake -S gpu -B "$out/gpu" -DBB_NET=OFF "-DBB_CPU_LIB=$PWD/$out/cpu/libbbcpu.so" >> "$out/gpu-cfg.log" 2>&1
ninja -C "$out/gpu" -j $J bbgpu > "$out/gpu-build.log" 2>&1 || { grep -v '^\[' "$out/gpu-build.log" | tail -30; exit 1; }
echo GPU_OK
mkdir -p "$out/bin" "$out/lib"
cp -f "$out/cpu/libbbcpu.so" "$out/gpu/libbbgpu.so" "$out/lib/"
cp -f "$llvm/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$out/lib/"
cc=("$llvm/bin/aarch64-linux-android$api-clang" -fuse-ld=lld)
mkdir -p "$out/atrac9"
for source in third_party/LibAtrac9/C/src/*.c; do
  "${cc[@]}" -std=c99 -O2 -g -w -fPIC -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
"$llvm/bin/llvm-ar" rcs "$out/libatrac9.a" "$out"/atrac9/*.o
links=(-L"$out/lib" -lbbgpu -lbbcpu -Wl,-rpath,'$ORIGIN/../lib' -rdynamic -pie)
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I"$prefix/include" -I. -Isrc \
  src/probe.c src/runtime*.c src/vulkan_smoke.c "$out/libatrac9.a" -lm "${links[@]}" \
  -o "$out/bin/bb-probe" 2> "$out/probe-build.log" || { tail -30 "$out/probe-build.log"; exit 1; }
echo PROBE_OK
ls -la "$out/bin/bb-probe" "$out/lib/"
