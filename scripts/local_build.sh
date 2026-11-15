#!/usr/bin/env bash
# divaft_Android 本地快速构建（ARM64 FEX harness，NDK Clang）。增量编译秒级，远快于 CI。
# 环境重置后跑此脚本恢复：装工具→从 ossfs 恢复 NDK→克隆→拉 FEX→构建→静态链接。
set -euo pipefail
WS=/root/userdata/workspace
PERSIST=$WS/divaft-persist
WORK=/root/divaft-work
NDK=/root/android-ndk-r30
# 1 工具
need=(); command -v cmake>/dev/null||need+=(cmake); command -v ninja>/dev/null||need+=(ninja-build)
command -v g++-14>/dev/null||need+=(g++-14); command -v qemu-aarch64>/dev/null||need+=(qemu-user)
command -v unzip>/dev/null||need+=(unzip)
[ ${#need[@]} -gt 0 ] && apt-get install -y -qq "${need[@]}" >/tmp/lb-tools.log 2>&1
# 2 NDK（从持久 zip 恢复）
if [ ! -d "$NDK" ]; then
  echo "恢复 NDK..."; cp "$PERSIST/ndk-r30-linux.zip" /root/ndk.zip 2>/dev/null || \
    python3 -c "
with open('$PERSIST/ndk-r30-linux.zip','rb') as f,open('/root/ndk.zip','wb') as g:
    while True:
        b=f.read(1<<20)
        if not b:break
        g.write(b)"
  cd /root && unzip -q ndk.zip
fi
export ANDROID_NDK_ROOT=$NDK
# 3 仓库
mkdir -p "$WORK"; cd "$WORK"
if [ ! -d repo/.git ]; then
  : "${GH_TOKEN:?需 GH_TOKEN}"
  git clone --quiet --depth 1 "https://x-access-token:${GH_TOKEN}@github.com/Ziran123lst/divaft_Android.git" repo
  git -C repo remote set-url origin https://github.com/Ziran123lst/divaft_Android.git
fi
cd repo
# 同步 staging 最新源码
for f in runtime/guest_bridge.cpp runtime/diva_boot.cpp runtime/diva_runtime.cpp; do
  [ -f "$WS/divaft-build/$f" ] && cp -f "$WS/divaft-build/$f" "$f"
done
# 4 FEX
[ -d third_party/fex/FEXCore ] || bash scripts/fetch-fex.sh >/tmp/lb-fex.log 2>&1
# 5 构建（首次全量，之后增量）
if [ ! -d build/fex-android ]; then bash scripts/configure-fex-android.sh >/tmp/lb-cfg.log 2>&1; fi
cmake --build build/fex-android --target fexcore-guest-harness -j4
# 6 静态链接
cd build/fex-android
bash -c 'L=$(ninja -t commands fexcore-guest-harness|tail -1); S=${L/-o Bin\/fexcore-guest-harness/-o Bin\/fexcore-guest-harness-static -static}; S=${S//-llog/}; eval "$S"'
echo "LOCAL_BUILD_OK $WORK/repo/build/fex-android/Bin/fexcore-guest-harness-static"
