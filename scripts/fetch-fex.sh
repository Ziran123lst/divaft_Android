#!/usr/bin/env bash
# divaft_Android — 拉取 FEX 源码（锁定 revision + 子模块）
# FEX 作为 CPU 翻译依赖。revision 与已验证 11/11 的配置一致。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
URL="https://github.com/FEX-Emu/FEX.git"
REV="ea43314f040633c76fb5891ff31d8a7c3dc533b8"
DEST=${FEX_SRC:-"$ROOT/third_party/fex"}

SUBMODULES=(
  External/unordered_dense
  External/rpmalloc
  External/xxhash
  External/fmt
  External/range-v3
  Source/Common/cpp-optparse
)

echo "fex.url = $URL"
echo "fex.rev = $REV"
echo "fex.dest = $DEST"
mkdir -p "$(dirname "$DEST")"
if [[ ! -d "$DEST/.git" ]]; then
  git clone --filter=blob:none --no-checkout "$URL" "$DEST"
fi
if ! git -C "$DEST" cat-file -e "$REV^{commit}" 2>/dev/null; then
  git -C "$DEST" fetch --depth 1 origin "$REV"
fi
git -C "$DEST" checkout --detach --force "$REV"
git -C "$DEST" clean -fdq
for sm in "${SUBMODULES[@]}"; do
  git -C "$DEST" submodule update --init --depth 1 "$sm"
done
echo "FEX_FETCH_OK $DEST @ $(git -C "$DEST" rev-parse --short HEAD)"
