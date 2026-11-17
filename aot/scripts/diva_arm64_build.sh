#!/bin/bash
# DIVA glibc ARM64 运行时构建（qemu 可跑）：libbbcpu + libbbgpu + bb-probe
set -euo pipefail
cd /root/divaft-work/bb-android
LLVM=/root/divaft-work/bb-recomp/.local-deps/llvm/x/LLVM-23.1.3-Linux-X64
export PATH=$LLVM/bin:$PATH
SR=$PWD/.local-deps/android/arm64-sysroot
HDR=$PWD/.local-deps/root/usr
TC=$PWD/tools/android/aarch64-toolchain.cmake
out=out/arm64
J=4
mkdir -p $out
# vulkan-headers-upgrade: sysroot 的 Vulkan 头太旧，升级到 1.4.363
BP=$PWD/.local-deps/vk-prefix
if ! grep -q eMesaKosmickrisp $SR/usr/include/vulkan/vulkan_enums.hpp 2>/dev/null; then
  cp -a $BP/include/vulkan $SR/usr/include/; cp -a $BP/include/vk_video $SR/usr/include/ 2>/dev/null || true
fi
# miniz（glibc arm64 版，装进 HDR）
if [[ ! -f $HDR/.done-miniz-glibc ]]; then
  cmake -S .local-deps/android/bionic-src/miniz-3.1.2 -B $out/miniz -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$TC -DBB_ARM64_SYSROOT=$SR -DBB_ARM64_HEADER_PREFIX=$HDR \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HDR -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON > $out/miniz-cfg.log 2>&1
  cmake --build $out/miniz -j $J > $out/miniz.log 2>&1
  cmake --install $out/miniz > $out/miniz-inst.log 2>&1
  touch $HDR/.done-miniz-glibc; echo MINIZ_OK
fi
cmake -S cpu -B $out/cpu -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DBB_ARM64_SYSROOT=$SR \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo > $out/cpu-cfg.log 2>&1 || { tail -15 $out/cpu-cfg.log; exit 1; }
cmake --build $out/cpu -j $J > $out/cpu-build.log 2>&1 || { tail -25 $out/cpu-build.log; exit 1; }
echo CPU_OK
cmake -S gpu -B $out/gpu -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DBB_ARM64_SYSROOT=$SR \
  -DBB_ARM64_HEADER_PREFIX=$HDR -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF -DBB_NET=OFF \
  -Dmagic_enum_DIR=$HDR/share/cmake/magic_enum -Dtsl-robin-map_DIR=$HDR/share/cmake/tsl-robin-map \
  -DVulkanMemoryAllocator_DIR=$HDR/share/cmake/VulkanMemoryAllocator \
  -DBoost_DIR=$HDR/lib/cmake/Boost-1.92.0 -Dboost_headers_DIR=$HDR/lib/cmake/boost_headers-1.92.0 \
  -Dminiz_DIR=$HDR/lib/cmake/miniz -DVulkanHeaders_DIR=$PWD/.local-deps/vk-prefix/share/cmake/VulkanHeaders -DSPIRV-Headers_DIR=$PWD/.local-deps/vk-prefix/share/cmake/SPIRV-Headers -DBB_CPU_LIB=$PWD/$out/cpu/libbbcpu.so > $out/gpu-cfg.log 2>&1 || { tail -20 $out/gpu-cfg.log; exit 1; }
cmake --build $out/gpu -j $J --target bbgpu > $out/gpu-build.log 2>&1 || { grep -v '^\[' $out/gpu-build.log | tail -30; exit 1; }
echo GPU_OK
mkdir -p $out/bin $out/lib
cp -f $out/cpu/libbbcpu.so $out/gpu/libbbgpu.so $out/lib/
# libatrac9
mkdir -p $out/atrac9
for source in third_party/LibAtrac9/C/src/*.c; do
  clang --target=aarch64-linux-gnu --sysroot=$SR -std=c99 -O2 -g -w -fPIC -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
llvm-ar rcs $out/libatrac9.a $out/atrac9/*.o
# bb-probe
export PKG_CONFIG_LIBDIR=$SR/usr/lib/aarch64-linux-gnu/pkgconfig:$SR/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$SR
pc=$(pkg-config --libs sdl3 x11 libavformat libavcodec libavutil libswscale libswresample libxxhash zlib 2>/dev/null || true)
clang --target=aarch64-linux-gnu --sysroot=$SR -fuse-ld=lld -std=c11 -O2 -g -Wall -Wextra -pthread \
  -I$HDR/include -I$SR/usr/include -I. -Isrc \
  src/probe.c src/runtime*.c src/vulkan_smoke.c $out/libatrac9.a \
  -L$PWD/$out/lib -L$HDR/lib -L$SR/usr/lib/aarch64-linux-gnu -lbbgpu -lbbcpu -Wl,-rpath,'$ORIGIN/../lib' -rdynamic -pie \
  $pc -lfmt -lZydis -lZycore -lminiz -lm -ldl -lstdc++ \
  -o $out/bin/bb-probe 2> $out/probe-build.log || { tail -30 $out/probe-build.log; exit 1; }
echo PROBE_OK
ls -la $out/bin/bb-probe $out/lib/
