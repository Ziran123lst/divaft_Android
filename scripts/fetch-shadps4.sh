#!/usr/bin/env bash
# divaft_Android — 拉取 shadPS4 源码 + shader_recompiler 所需 externals（CI 用）
# 目标：交叉编译 shader_recompiler（GCN→IR→SPIR-V）为 ARM64 静态库。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
DEST=${SHADPS4_SRC:-"$ROOT/third_party/shadps4"}
REV=${SHADPS4_REV:-main}   # 可锁定到快照对应 commit

echo "shadps4.rev = $REV"
echo "shadps4.dest = $DEST"
mkdir -p "$(dirname "$DEST")"
if [[ ! -d "$DEST/.git" ]]; then
  git clone --filter=blob:none --no-checkout https://github.com/shadps4-emu/shadPS4.git "$DEST"
fi
git -C "$DEST" fetch --depth 1 origin "$REV"
git -C "$DEST" checkout --detach --force FETCH_HEAD

# shader_recompiler 所需 externals（浅克隆）。sirit=SPIR-V 发射（含嵌套 SPIRV-Headers，需 recursive）。
# gcn 为 vendored 随 checkout 自带；xxhash 小写；spirv-headers 在 sirit 内嵌套。
SUBMODULES=(
  externals/fmt
  externals/magic_enum
  externals/half
  externals/robin-map
  externals/ext-boost
  externals/vulkan-headers
  externals/xxhash
  externals/spdlog
  externals/json
  externals/xbyak
)
# Zydis（x86 解码器，SRT pass 用；可移植，ARM64 可编）含嵌套 Zycore，recursive。
echo "=== submodule externals/zydis (recursive) ==="
git -C "$DEST" submodule update --init --recursive --depth 1 externals/zydis || \
  echo "warn: zydis 子模块初始化失败"
for sm in "${SUBMODULES[@]}"; do
  echo "=== submodule $sm ==="
  git -C "$DEST" submodule update --init --depth 1 "$sm" || \
    echo "warn: $sm 子模块初始化失败（可能需手动补）"
done
# sirit 及其嵌套 SPIRV-Headers
echo "=== submodule externals/sirit (recursive) ==="
git -C "$DEST" submodule update --init --recursive --depth 1 externals/sirit || \
  echo "warn: sirit 子模块初始化失败"
echo "FETCH_SHADPS4_OK $DEST"