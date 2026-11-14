#!/usr/bin/env bash
# divaft_Android — 本地构建一键恢复脚本（环境重置后跑这个即可）
#
# 持久化布局（全部在持久工作区，重置不丢）：
#   workspace/divaft-build/      源码 staging（与 GitHub 同步）
#   workspace/divaft-persist/    产物库：SPIR-V、日志、二进制、shadps4 源码缓存
#   workspace/sbo_shaders/       SBO 原始文件（用户上传）
#   workspace/eboot_shaders_full/ eboot 提取的 shader
# 临时构建区 /root/divaft-work（overlay 本地盘，重置会清——只放可再生的构建中间物）
#
# 为什么构建必须在临时盘：工作区是 ossfs 顺序追加写文件系统，编译器/汇编器随机写
# .o 会失败；git pack 随机写同样失败。所以「构建在临时盘、产物立即持久化」。
#
# 用法: GH_TOKEN=<github token> bash scripts/rebuild_local.sh [--no-convert]
set -euo pipefail
WS=/root/userdata/workspace
PERSIST="$WS/divaft-persist"
WORK=/root/divaft-work
REPO="$WORK/repo"
STAGING="$WS/divaft-build"
mkdir -p "$PERSIST" "$WORK"

echo "== [1/6] 工具链 =="
need=()
command -v cmake >/dev/null || need+=(cmake)
command -v ninja >/dev/null || need+=(ninja-build)
command -v g++-14 >/dev/null || need+=(g++-14)
command -v spirv-val >/dev/null || need+=(spirv-tools)
if [ ${#need[@]} -gt 0 ]; then
  apt-get install -y -qq "${need[@]}" > "$PERSIST/last-apt.log" 2>&1
fi
echo "cmake=$(cmake --version | head -1) g++14=$(g++-14 --version | head -1)"

echo "== [2/6] 仓库 =="
if [ ! -d "$REPO/.git" ]; then
  : "${GH_TOKEN:?需要 GH_TOKEN 环境变量}"
  git clone --quiet --depth 1 "https://x-access-token:${GH_TOKEN}@github.com/Ziran123lst/divaft_Android.git" "$REPO"
  git -C "$REPO" remote set-url origin https://github.com/Ziran123lst/divaft_Android.git
fi
# staging 里可能有未推送的更新，覆盖到构建副本
for f in shader_build/sbo_translate_test.cpp shader_build/CMakeLists.txt shader_build/shader_stubs.cpp \
         scripts/sbo_extract.py scripts/eboot_shaders_extract.py scripts/patch_interp_fallback.py; do
  [ -f "$STAGING/$f" ] && python3 -c "
import sys
src,dst=sys.argv[1],sys.argv[2]
open(dst,'wb').write(open(src,'rb').read())" "$STAGING/$f" "$REPO/$f"
done
echo "repo HEAD: $(git -C "$REPO" log --oneline -1)"

echo "== [3/6] shadPS4 依赖（优先用持久缓存） =="
CACHE="$PERSIST/shadps4-src-cache.tar.gz"
if [ ! -f "$REPO/third_party/shadps4/externals/sirit/CMakeLists.txt" ]; then
  if [ -f "$CACHE" ]; then
    echo "从持久缓存恢复 shadps4 源码..."
    mkdir -p "$REPO/third_party"
    tar xzf "$CACHE" -C "$REPO/third_party"
  else
    bash "$REPO/scripts/fetch-shadps4.sh" > "$PERSIST/last-fetch.log" 2>&1
    echo "制作持久缓存（首次较慢，之后重置免拉取）..."
    tar czf "$WORK/shadps4-cache.tar.gz" -C "$REPO/third_party" shadps4
    python3 -c "
import sys
src,dst=sys.argv[1],sys.argv[2]
with open(src,'rb') as f, open(dst,'wb') as g:
    while True:
        b=f.read(1<<20)
        if not b: break
        g.write(b)" "$WORK/shadps4-cache.tar.gz" "$CACHE"
    rm -f "$WORK/shadps4-cache.tar.gz"
  fi
fi
echo "shadps4: $(git -C "$REPO/third_party/shadps4" log --oneline -1 2>/dev/null || echo snapshot)"

echo "== [4/6] 构建（x86 原生，验证用；ARM64 走 CI） =="
python3 "$REPO/scripts/patch_interp_fallback.py" "$REPO/third_party/shadps4"
cmake -S "$REPO/shader_build" -B "$REPO/build/shader-x86" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 > "$PERSIST/last-cfg.log" 2>&1
cmake --build "$REPO/build/shader-x86" -j4 > "$PERSIST/last-build.log" 2>&1
echo "BUILD_OK"

if [ "${1:-}" = "--no-convert" ]; then
  echo "跳过转换（--no-convert）"; exit 0
fi

echo "== [5/6] 提取 + 全量转换 =="
mkdir -p "$WORK/xdg"
python3 "$REPO/scripts/sbo_extract.py" "$WS/sbo_shaders/shader_ps4.sbo.txt" "$WORK/sbo_out/ps4" | tail -2
export XDG_DATA_HOME="$WORK/xdg"
"$REPO/build/shader-x86/sbo_translate_test" "$WORK/sbo_out/ps4" "$WORK/spv_out" > "$WORK/sbo-translate.log" 2>&1 || true
grep -E 'SBO_TRANSLATE_(SUMMARY|TEST)' "$WORK/sbo-translate.log" || true
"$REPO/build/shader-x86/sbo_translate_test" "$WS/eboot_shaders_full" "$WORK/spv_eboot" > "$WORK/eboot-translate.log" 2>&1 || true
grep -E 'SBO_TRANSLATE_(SUMMARY|TEST)' "$WORK/eboot-translate.log" || true

echo "== [6/6] 产物持久化（顺序写回工作区） =="
tar czf "$WORK/artifacts.tar.gz" -C "$WORK" spv_out spv_eboot sbo-translate.log eboot-translate.log \
  -C "$REPO/build/shader-x86" sbo_translate_test shader_translate_test
python3 -c "
import sys
src,dst=sys.argv[1],sys.argv[2]
with open(src,'rb') as f, open(dst,'wb') as g:
    while True:
        b=f.read(1<<20)
        if not b: break
        g.write(b)" "$WORK/artifacts.tar.gz" "$PERSIST/artifacts.tar.gz"
rm -f "$WORK/artifacts.tar.gz"
echo "产物已持久化: $PERSIST/artifacts.tar.gz"
echo "REBUILD_ALL_DONE"
