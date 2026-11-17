#!/bin/bash
# glibc ARM64 运行时精简依赖（sysroot 已有 fmt/zydis/SDL3/ffmpeg/X11）
set -euo pipefail
cd /root/divaft-work/bb-android
LLVM=/root/divaft-work/bb-recomp/.local-deps/llvm/x/LLVM-23.1.3-Linux-X64
export PATH=$LLVM/bin:$PATH
SR=$PWD/.local-deps/android/arm64-sysroot
HDR=$PWD/.local-deps/root/usr
TC=$PWD/tools/android/aarch64-toolchain.cmake
src=$PWD/.local-deps/glibc-src; bld=$PWD/.local-deps/glibc-build
M=https://ghfast.top/https://github.com
mkdir -p "$HDR" "$src" "$bld"
fetch(){ local dir=$src/$1; [[ -d $dir ]] && return 0; curl -fsSL --retry 3 "$2" -o "$src/$1.tar" && mkdir -p "$dir.tmp" && tar -xf "$src/$1.tar" -C "$dir.tmp" --strip-components=1 && mv "$dir.tmp" "$dir" && rm "$src/$1.tar"; }
fetch Vulkan-Headers-1.4.363 $M/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz
fetch SPIRV-Headers-1.4.309 $M/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.309.0.tar.gz
fetch magic_enum-0.9.7 $M/Neargye/magic_enum/archive/refs/tags/v0.9.7.tar.gz
fetch robin-map-1.3.0 $M/Tessil/robin-map/archive/refs/tags/v1.3.0.tar.gz
fetch VMA-3.2.1 $M/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.2.1.tar.gz
fetch miniz-3.1.2 $M/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
fetch boost_1_92_0 $M/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.gz
# Vulkan/SPIRV 头装到独立前缀（供 cmake config），并升级 sysroot 头
cmi(){ local name=$1 sdir=$2 prefix=$3; shift 3; [[ -f $prefix/.done-$name ]] && return 0;
  cmake -S "$sdir" -B "$bld/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" "$@" >"$bld/$name-cfg.log" 2>&1
  cmake --install "$bld/$name" >"$bld/$name-inst.log" 2>&1; touch $prefix/.done-$name; echo "done $name"; }
VKP=$PWD/.local-deps/vk-prefix; mkdir -p $VKP
cmi vulkan-headers "$src/Vulkan-Headers-1.4.363" $VKP
cmi spirv-headers "$src/SPIRV-Headers-1.4.309" $VKP
cp -a $VKP/include/vulkan $SR/usr/include/; cp -a $VKP/include/vk_video $SR/usr/include/ 2>/dev/null || true
cmi magic_enum "$src/magic_enum-0.9.7" "$HDR" -DMAGIC_ENUM_OPT_INSTALL=ON -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
cmi robin-map "$src/robin-map-1.3.0" "$HDR"
cmi VMA "$src/VMA-3.2.1" "$HDR"
if [[ ! -d $HDR/include/boost ]]; then
  cp -a "$src/boost_1_92_0/boost" "$HDR/include/"
  mkdir -p "$HDR/lib/cmake/Boost-1.92.0" "$HDR/lib/cmake/boost_headers-1.92.0"
  printf 'add_library(boost_headers INTERFACE)\nadd_library(Boost::headers ALIAS boost_headers)\nset_target_properties(boost_headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")\n' > "$HDR/lib/cmake/boost_headers-1.92.0/boost_headers-config.cmake"
  printf 'include("${CMAKE_CURRENT_LIST_DIR}/../boost_headers-1.92.0/boost_headers-config.cmake")\nset(Boost_FOUND TRUE)\n' > "$HDR/lib/cmake/Boost-1.92.0/BoostConfig.cmake"
  echo "done boost-headers"
fi
# miniz（glibc arm64）
if [[ ! -f $HDR/.done-miniz-glibc ]]; then
  cmake -S "$src/miniz-3.1.2" -B "$bld/miniz" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DBB_ARM64_SYSROOT=$SR \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HDR -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON >"$bld/miniz-cfg.log" 2>&1
  cmake --build "$bld/miniz" -j4 >"$bld/miniz.log" 2>&1
  cmake --install "$bld/miniz" >"$bld/miniz-inst.log" 2>&1; touch $HDR/.done-miniz-glibc; echo "done miniz"
fi
echo GLIBC_DEPS_OK
