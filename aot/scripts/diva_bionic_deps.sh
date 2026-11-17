#!/bin/bash
# DIVA 版 bionic 依赖（跳过在线模块 OpenSSL/protobuf）。在 bb-android 仓库根运行。
set -euo pipefail
cd /root/divaft-work/bb-android
root=$PWD/.local-deps/android
ndk=${BB_NDK:-/root/android-ndk-r30}
api=30
prefix=$root/bionic-prefix src=$root/bionic-src bld=$root/bionic-build
headers=$PWD/.local-deps/root/usr
J=4
M=https://ghfast.top/https://github.com
mkdir -p "$prefix" "$src" "$bld" "$headers"
fetch(){ local dir=$src/$1; [[ -d $dir ]] && return 0; curl -fsSL --retry 3 "$2" -o "$src/$1.tar" && mkdir -p "$dir.tmp" && tar -xf "$src/$1.tar" -C "$dir.tmp" --strip-components=1 && mv "$dir.tmp" "$dir" && rm "$src/$1.tar"; }
cm(){ local name=$1 sdir=$2; shift 2; [[ -f $prefix/.done-$name ]] && return 0;
  cmake -S "$sdir" -B "$bld/$name" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" "$@" >"$bld/$name-cfg.log" 2>&1
  cmake --build "$bld/$name" -j $J >"$bld/$name.log" 2>&1 || { tail -20 "$bld/$name.log"; exit 1; }
  cmake --install "$bld/$name" >"$bld/$name-inst.log" 2>&1
  touch $prefix/.done-$name; echo "done $name"; }
fetch fmt-10.1.1 $M/fmtlib/fmt/archive/refs/tags/10.1.1.tar.gz
fetch zycore-1.5.2 $M/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz
fetch zydis-4.1.1 $M/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz
fetch xxhash-0.8.3 $M/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
fetch miniz-3.1.2 $M/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
fetch Vulkan-Headers-1.4.363 $M/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz
fetch SPIRV-Headers-1.4.309 $M/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.309.0.tar.gz
fetch magic_enum-0.9.7 $M/Neargye/magic_enum/archive/refs/tags/v0.9.7.tar.gz
fetch robin-map-1.3.0 $M/Tessil/robin-map/archive/refs/tags/v1.3.0.tar.gz
fetch VMA-3.2.1 $M/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.2.1.tar.gz
fetch ffmpeg-n7.1.5 $M/FFmpeg/FFmpeg/archive/refs/tags/n7.1.5.tar.gz
fetch boost_1_92_0 $M/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.gz
cm vulkan-headers "$src/Vulkan-Headers-1.4.363"
cm spirv-headers "$src/SPIRV-Headers-1.4.309"
cm fmt "$src/fmt-10.1.1" -DFMT_TEST=OFF -DFMT_DOC=OFF
cm zycore "$src/zycore-1.5.2" -DBUILD_SHARED_LIBS=OFF
[[ -f $src/zydis-4.1.1/dependencies/zycore/CMakeLists.txt ]] || cp -a "$src/zycore-1.5.2/." "$src/zydis-4.1.1/dependencies/zycore/"
cm zydis "$src/zydis-4.1.1" -DBUILD_SHARED_LIBS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_DOXYGEN=OFF
cm xxhash "$src/xxhash-0.8.3/cmake_unofficial" -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cm miniz "$src/miniz-3.1.2" -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON
# header-only 到 headers 目录（build_bionic.sh 期望路径）
cm magic_enum "$src/magic_enum-0.9.7" -DMAGIC_ENUM_OPT_INSTALL=ON -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$headers"
cm robin-map "$src/robin-map-1.3.0" -DCMAKE_INSTALL_PREFIX="$headers"
cm VMA "$src/VMA-3.2.1" -DCMAKE_INSTALL_PREFIX="$headers"
# Boost：纯头文件安装 + 手写 cmake config（asio/container/icl 均 header-only）
if [[ ! -d $headers/include/boost ]]; then
  cp -a "$src/boost_1_92_0/boost" "$headers/include/"
  mkdir -p "$headers/lib/cmake/Boost-1.92.0" "$headers/lib/cmake/boost_headers-1.92.0"
  cat > "$headers/lib/cmake/boost_headers-1.92.0/boost_headers-config.cmake" <<'CM'
add_library(boost_headers INTERFACE)
add_library(Boost::headers ALIAS boost_headers)
set_target_properties(boost_headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
CM
  cat > "$headers/lib/cmake/Boost-1.92.0/BoostConfig.cmake" <<'CM'
include("${CMAKE_CURRENT_LIST_DIR}/../boost_headers-1.92.0/boost_headers-config.cmake")
set(Boost_FOUND TRUE)
set(BOOST_VERSION 1.92.0)
CM
  echo "done boost-headers"
fi
# ffmpeg（最小配置）
if [[ ! -f $prefix/.done-ffmpeg ]]; then
  mkdir -p "$bld/ffmpeg" && cd "$bld/ffmpeg"
  tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
  "$src/ffmpeg-n7.1.5/configure" --prefix="$prefix" --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$tc/bin/aarch64-linux-android$api-clang" --cxx="$tc/bin/aarch64-linux-android$api-clang++" \
    --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-network \
    --disable-avdevice --disable-avfilter --disable-autodetect --disable-mediacodec --disable-jni \
    --disable-everything --enable-decoder=aac,ac3,atrac3,atrac3p,mp3,pcm_s16le,pcm_s16be,h264,hevc,mpeg2video,mpeg4,vp8,vp9 \
    --enable-demuxer=aac,ac3,atrac3,atrac3p,mp3,mov,matroska,avi,mpegts,mpegps,wav,flv \
    --enable-parser=aac,ac3,h264,hevc,mpegaudio,mpeg4video,vp8,vp9 \
    --enable-protocol=file --enable-swresample --enable-swscale >"$bld/ffmpeg-cfg.log" 2>&1 || { tail -20 "$bld/ffmpeg-cfg.log"; exit 1; }
  make -j$J >"$bld/ffmpeg.log" 2>&1 || { tail -20 "$bld/ffmpeg.log"; exit 1; }
  make install >"$bld/ffmpeg-inst.log" 2>&1
  touch "$prefix/.done-ffmpeg"; echo "done ffmpeg"; cd /root/divaft-work/bb-android
fi
echo DEPS_OK
