#!/bin/bash
# 把 shadPS4 完整 Ngs2 DSP 合成器移植进 bb-android 运行时（替换最小静音桩）。
# Ngs2 自包含（仅依赖 common/logging+error_codes+libs.h，无 audio_core），渲染进 guest 缓冲；
# 音频输出由 runtime_audio.c 的 AudioOut（SDL3/FIFO/AAudio）separately 处理。
set -euo pipefail
BA=${DIVA_BB_ANDROID:-/root/divaft-work/bb-android}
SP4=${DIVA_SP4_SRC:-/tmp/sp4count/shadps4/src}
N=$SP4/core/libraries/ngs2
[ -d "$N" ] || { echo "需先解 shadPS4 源码到 $SP4（divaft-persist/shadps4-src-cache.tar.gz）"; exit 1; }
cd "$BA"
mkdir -p gpu/shadps4/core/libraries/ngs2
cp "$N"/*.cpp "$N"/*.h gpu/shadps4/core/libraries/ngs2/
python3 - <<'PY'
import os
ba=os.environ.get('DIVA_BB_ANDROID','/root/divaft-work/bb-android')
s=open(ba+'/gpu/CMakeLists.txt').read()
if 'NGS2' not in s:
    s=s.replace('file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)',
                'file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)\nfile(GLOB NGS2 CONFIGURE_DEPENDS shadps4/core/libraries/ngs2/*.cpp)')
    s=s.replace('shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}',
                'shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER} ${NGS2}')
    open(ba+'/gpu/CMakeLists.txt','w').write(s)
s=open(ba+'/gpu/shim/bbgpu.cpp').read()
if 'Ngs2::RegisterLib' not in s:
    s=s.replace('namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }',
                'namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }\nnamespace Libraries::Ngs2 { void RegisterLib(Core::Loader::SymbolsResolver* sym); }')
    s=s.replace('    Libraries::AvPlayer::RegisterLib(&resolver);',
                '    Libraries::AvPlayer::RegisterLib(&resolver);\n    Libraries::Ngs2::RegisterLib(&resolver);')
    open(ba+'/gpu/shim/bbgpu.cpp','w').write(s)
# 移除最小静音桩（让 bbgpu 真实 Ngs2 生效）
s=open(ba+'/src/runtime.c').read()
s=s.replace('    uintptr_t ngs2 = runtime_ngs2_resolve(name);\n    if (ngs2) return ngs2;\n','')
open(ba+'/src/runtime.c','w').write(s)
s=open(ba+'/src/runtime.h').read()
s=s.replace('uintptr_t runtime_ngs2_resolve(const char *name);\n','')
open(ba+'/src/runtime.h','w').write(s)
print('Ngs2 ported (real shadPS4 DSP)')
PY
echo NGS2_PORT_OK
