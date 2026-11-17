#!/bin/bash
# 幂等应用 DIVA 适配补丁（bb-recomp + bb-android）。环境重置后重跑。
set -euo pipefail
BR=${DIVA_BB_RECOMP:-/root/divaft-work/bb-recomp}; BA=${DIVA_BB_ANDROID:-/root/divaft-work/bb-android}
BOOT_LINKED=${DIVA_BOOT_LINKED:-/root/userdata/workspace/divaft-persist/diva-aot/boot-linked.bin}
SP4_SRC=${DIVA_SP4_SRC:-/tmp/sp4count/shadps4/src}
CONTRACT=${DIVA_CONTRACT:-/root/userdata/workspace/divaft-build/runtime/data/contract.tsv}
# ---- bb-recomp: fetch-fex.sh 镜像 ----
python3 - "$BR/fetch-fex.sh" <<'EOF'
import sys
s=open(sys.argv[1]).read()
old='    git -C "$dest/$path" init -q && git -C "$dest/$path" fetch -q --depth 1 "$url" "$sha"'
if 'ghfast.top/$url' not in s:
    new='    [[ $url == https://github.com/* ]] && url="https://ghfast.top/$url"\n'+old
    assert s.count(old)==1; open(sys.argv[1],'w').write(s.replace(old,new)); print('fetch-fex mirror patched')
EOF
# ---- bb-recomp: masked load/store lowering ----
python3 - "$BR/src/lower_vector.cpp" <<'EOF'
import sys
s=open(sys.argv[1]).read()
if 'OP_VLOADVECTORMASKED' in s: print('masked ops already present'); sys.exit()
anchor='''  case OP_VSTORENONTEMPORAL: {
    auto* o = op->C<IROp_VStoreNonTemporal>();
    storeVal(B.CreateAdd(A(o->Addr), c64((int64_t)o->Offset)), A(o->Value), size, true);
    return true;
  }
'''
add='''  case OP_VLOADVECTORMASKED: {
    // AVX vmaskmovps/pd: per-lane sign-bit mask gates the load; inactive lanes read as zero.
    auto* o = op->C<IROp_VLoadVectorMasked>();
    Value* p = B.CreateIntToPtr(memAddr(o->Addr, o->Offset, o->OffsetType, o->OffsetScale), Ptr);
    Value* ml = lanesI(A(o->Mask), size, e);
    Value* m = B.CreateICmpSLT(ml, ConstantAggregateZero::get(ml->getType()));
    Value* ld = B.CreateMaskedLoad(ml->getType(), p, Align(1), m, ConstantAggregateZero::get(ml->getType()));
    return ret(ld);
  }
  case OP_VSTOREVECTORMASKED: {
    auto* o = op->C<IROp_VStoreVectorMasked>();
    Value* p = B.CreateIntToPtr(memAddr(o->Addr, o->Offset, o->OffsetType, o->OffsetScale), Ptr);
    Value* ml = lanesI(A(o->Mask), size, e);
    Value* m = B.CreateICmpSLT(ml, ConstantAggregateZero::get(ml->getType()));
    B.CreateMaskedStore(lanesI(A(o->Data), size, e), p, Align(1), m);
    return true;
  }
'''
assert s.count(anchor)==1; open(sys.argv[1],'w').write(s.replace(anchor,anchor+add)); print('masked ops inserted')
EOF
# ---- bb-android: probe.c 桩模式 ----
python3 - "$BA/src/probe.c" <<'EOF'
import sys
s=open(sys.argv[1]).read()
if 'BB_IMPORT_STUBS' in s: print('stub mode already present'); sys.exit()
old='static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {\n    if (id >= import_count) fail("bad import trap index");'
new='''static ABI uint64_t unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    if (getenv("BB_IMPORT_STUBS")) {
        static unsigned char seen[8192];
        if (id < 8192 && !seen[id]) { seen[id] = 1;
            fprintf(stderr, "STUB import: %s (%s) arg0=0x%llx\\n", names[id], runtime_import_name(names[id]), (unsigned long long)argument); }
        return 0;
    }'''
assert s.count(old)==1; open(sys.argv[1],'w').write(s.replace(old,new)); print('stub mode patched')
EOF
# ---- bb-android: runtime.c DIVA scope 变体 + 字典 ----
python3 - "$BA/src/runtime.c" <<'EOF'
import sys,re
s=open(sys.argv[1]).read()
if 'import_names_diva.inc' in s: print('runtime.c already patched'); sys.exit()
old='#include "import_names.inc"'
assert s.count(old)==1
s=s.replace(old,old+'\n#include "import_names_diva.inc"')
pairs=[('bzQExy189ZI#q#q','bzQExy189ZI#q#n'),('8G2LB+A3rzg#q#q','8G2LB+A3rzg#q#n'),('tsvEmnenz48#q#q','tsvEmnenz48#q#n'),('uMei1W9uyNo#q#q','uMei1W9uyNo#q#n'),('H2e8t5ScQGc#q#q','H2e8t5ScQGc#libSceLibcInternal'),('3GPpjQdAMTw#q#q','3GPpjQdAMTw#q#n'),('9rAeANT2tyE#q#q','9rAeANT2tyE#q#n'),('2emaaluWzUw#q#q','2emaaluWzUw#q#n'),('Ou3iL1abvng#p#J','Ou3iL1abvng#m#I'),('8zTFvBIAIN8#q#q','8zTFvBIAIN8#q#n'),('Q3VBxCXhUHs#q#q','Q3VBxCXhUHs#q#n'),('+P6FRGH4LfA#q#q','+P6FRGH4LfA#q#n'),('DfivPArhucg#q#q','DfivPArhucg#q#n'),('j4ViWNHEgww#q#q','j4ViWNHEgww#q#n'),('vNe1w4diLCs#p#J','vNe1w4diLCs#m#I'),('959qrazPIrg#p#J','959qrazPIrg#m#I'),('p5EcQeEeJAE#p#J','p5EcQeEeJAE#m#I'),('f7uOxY9mM1U#p#J','f7uOxY9mM1U#m#I')]
n=0
for bb,dv in pairs:
    pat=re.compile(r'^([ \t]*)(if \(!strcmp\(name, "'+re.escape(bb)+r'"\)\) return .+;)$',re.M)
    m=pat.search(s)
    if not m: continue
    s=s[:m.end()]+'\n'+m.group(1)+m.group(2).replace(bb,dv)+s[m.end():]; n+=1
open(sys.argv[1],'w').write(s); print('runtime.c patched',n,'variants')
EOF
# ---- bb-android: savedata 标题 ID ----
sed -i 's/"CUSA03173","CUSA03179"}/"CUSA03173","CUSA03179","CUSA08053"}/' "$BA/src/runtime_savedata.c"
# ---- bb-android: DIVA NID 字典生成 ----
python3 - "$BA" <<'EOF'
import sys,re,subprocess,glob,os
ba=sys.argv[1]
out=subprocess.run(['strings',os.environ.get('DIVA_BOOT_LINKED','/root/userdata/workspace/divaft-persist/diva-aot/boot-linked.bin')],capture_output=True,text=True).stdout
scoped=sorted(set(l for l in out.splitlines() if re.fullmatch(r'[A-Za-z0-9+/=_-]{11}#.*',l)))
contract={}
for line in open(os.environ.get('DIVA_CONTRACT','/root/userdata/workspace/divaft-build/runtime/data/contract.tsv')):
    p=line.rstrip('\n').split('\t')
    if len(p)>=3 and p[0] and not p[0].startswith('#'): contract[p[0]]=p[2]
nid2name={}
for f in glob.glob(os.environ.get('DIVA_SP4_SRC','/tmp/sp4count/shadps4/src')+'/**/*.cpp',recursive=True):
    for m in re.finditer(r'LIB_FUNCTION\("([^"]+)",\s*"[^"]+",\s*\d+,\s*"[^"]+",\s*([A-Za-z0-9_]+)\)',open(f,errors='ignore').read()):
        nid2name[m.group(1)]=m.group(2)
with open(ba+'/src/import_names_diva.inc','w') as f:
    f.write('/* DIVA CUSA08053 scoped NID -> symbol (generated) */\n')
    n=0
    for s in scoped:
        nid=s.split('#')[0]
        name=contract.get(nid) or nid2name.get(nid)
        if name: f.write(f'{{"{s}","{name}"}},\n'); n+=1
print('diva dictionary:',n,'entries')
EOF
echo PATCHES_OK
# ---- bb-android: 全量 NID 别名（DIVA scope→血源 scope，扫镜像753+全部src/*.c）----
python3 - "$BA" <<'EOF'
import sys,re,glob,subprocess,os
ba=sys.argv[1]
bb={}
for f in glob.glob(ba+'/src/*.c')+[ba+'/src/import_names.inc']:
    for m in re.finditer(r'"([A-Za-z0-9+/=_-]{11}#[^"]+)"',open(f,errors='ignore').read()):
        s=m.group(1); bb.setdefault(s.split('#')[0],s)
out=subprocess.run(['strings',os.environ.get('DIVA_BOOT_LINKED','/root/userdata/workspace/divaft-persist/diva-aot/boot-linked.bin')],capture_output=True,text=True).stdout
scoped=sorted(set(l for l in out.splitlines() if re.fullmatch(r'[A-Za-z0-9+/=_-]{11}#.*',l)))
aliases=[(ds,bb[ds.split('#')[0]]) for ds in scoped if ds.split('#')[0] in bb and bb[ds.split('#')[0]]!=ds]
with open(ba+'/src/diva_nid_alias.inc','w') as f:
    f.write('static const struct { const char* diva; const char* bb; } diva_nid_alias[] = {\n')
    for d,b in sorted(aliases): f.write(f'  {{"{d}","{b}"}},\n')
    f.write('};\n')
s=open(ba+'/src/runtime.c').read()
if 'diva_nid_alias' not in s:
    old='uintptr_t runtime_resolve(const char *name, int is_data) {\n    if (!(capabilities & 1)) return 0;'
    new='''uintptr_t runtime_resolve(const char *name, int is_data) {
    if (!(capabilities & 1)) return 0;
    {
#include "diva_nid_alias.inc"
        for (size_t i = 0; i < sizeof(diva_nid_alias)/sizeof(*diva_nid_alias); ++i)
            if (!strcmp(name, diva_nid_alias[i].diva)) { name = diva_nid_alias[i].bb; break; }
    }'''
    assert s.count(old)==1; open(ba+'/src/runtime.c','w').write(s.replace(old,new))
print('alias table:',len(aliases))
EOF
echo ALIAS_PATCH_ADDED
# ---- bb-recomp: VPCMPISTRX lowering + C helper ----
SD="$(cd "$(dirname "$0")" && pwd)"
cp "$SD/bbr_pcmpistrx.c" "$BR/helpers/bbr_pcmpistrx.c"
grep -q 'bbr_pcmpistrx.c' "$BR/helpers/bbr_x87_all.c" || echo '#include "bbr_pcmpistrx.c"' >> "$BR/helpers/bbr_x87_all.c"
python3 - "$BR/src/lower_vector.cpp" <<'EOF'
import sys
s=open(sys.argv[1]).read()
if 'OP_VPCMPISTRX' in s: print('pcmpistrx already present'); sys.exit()
anchor='''  case OP_VSTOREVECTORMASKED: {
    auto* o = op->C<IROp_VStoreVectorMasked>();
    Value* p = B.CreateIntToPtr(memAddr(o->Addr, o->Offset, o->OffsetType, o->OffsetScale), Ptr);
    Value* ml = lanesI(A(o->Mask), size, e);
    Value* m = B.CreateICmpSLT(ml, ConstantAggregateZero::get(ml->getType()));
    B.CreateMaskedStore(lanesI(A(o->Data), size, e), p, Align(1), m);
    return true;
  }
'''
add='''  case OP_VPCMPISTRX: {
    // SSE4.2 PCMPISTRI/M intermediate result via a C helper (FEX semantics).
    auto* o = op->C<IROp_VPCMPISTRX>();
    Value* lhs = fromV(A(o->LHS));
    Value* rhs = fromV(A(o->RHS));
    Value* ll = B.CreateTrunc(lhs, I64);
    Value* lh = B.CreateTrunc(B.CreateLShr(lhs, 64), I64);
    Value* rl = B.CreateTrunc(rhs, I64);
    Value* rh = B.CreateTrunc(B.CreateLShr(rhs, 64), I64);
    Value* r = B.CreateCall(m.helper("bbr_pcmpistrx", I32, {I64, I64, I64, I64, I32}), {ll, lh, rl, rh, c64(o->Control)});
    def(node, zx(r));
    return true;
  }
'''
assert s.count(anchor)==1; open(sys.argv[1],'w').write(s.replace(anchor,anchor+add)); print('pcmpistrx lowering inserted')
EOF
# ---- bb-android: DIVA HLE 模块（Ngs2/misc/kernel）+ 接线 ----
cp "$SD/runtime_diva_misc.c.diva" "$BA/src/runtime_diva_misc.c"
cp "$SD/runtime_kernel.c.diva" "$BA/src/runtime_kernel.c"
python3 - "$BA" <<'EOF'
import sys
ba=sys.argv[1]
s=open(ba+'/src/runtime.h').read()
if 'runtime_diva_misc_resolve' not in s:
    s=s.replace('uintptr_t runtime_audio_resolve(const char *name);',
        'uintptr_t runtime_audio_resolve(const char *name);\nuintptr_t runtime_diva_misc_resolve(const char *name);')
    open(ba+'/src/runtime.h','w').write(s)
s=open(ba+'/src/runtime.c').read()
if 'runtime_diva_misc_resolve' not in s:
    old='''    uintptr_t audio = runtime_audio_resolve(name);
    if (audio) return audio;'''
    new=old+'''
    uintptr_t misc = runtime_diva_misc_resolve(name);
    if (misc) return misc;'''
    assert s.count(old)==1; open(ba+'/src/runtime.c','w').write(s.replace(old,new))
print('misc wired (real Ngs2 via shadPS4)')
EOF
# ---- bb-android: 最小 Ngs2 HLE（配套旧 libbbgpu 无 shadPS4 Ngs2 时；有效句柄解锁音频初始化）----
cp "$SD/runtime_ngs2.c.diva" "$BA/src/runtime_ngs2.c"
python3 - "$BA" <<'PYEOF'
import sys
ba=sys.argv[1]
s=open(ba+'/src/runtime.h').read()
if 'runtime_ngs2_resolve' not in s:
    s=s.replace('uintptr_t runtime_diva_misc_resolve(const char *name);',
        'uintptr_t runtime_diva_misc_resolve(const char *name);\nuintptr_t runtime_ngs2_resolve(const char *name);')
    open(ba+'/src/runtime.h','w').write(s)
s=open(ba+'/src/runtime.c').read()
if 'runtime_ngs2_resolve' not in s:
    old='    uintptr_t misc = runtime_diva_misc_resolve(name);\n    if (misc) return misc;'
    new='    uintptr_t ngs2 = runtime_ngs2_resolve(name);\n    if (ngs2) return ngs2;\n'+old
    assert s.count(old)==1
    open(ba+'/src/runtime.c','w').write(s.replace(old,new))
print('ngs2 wired (minimal HLE)')
PYEOF
echo DIVA_HLE_PATCHES_OK

# ---- bb-android: AOT 崩溃诊断增强（guest gregs + host regs + 内存探针）----
if ! grep -q 'AOT: gregs:' "$BA/cpu/guest_cpu_aot.cpp"; then
  git -C "$BA" apply "$SD/aot-crash-diagnostics.patch" 2>/dev/null || patch -d "$BA" -p1 -s < "$SD/aot-crash-diagnostics.patch"
  echo "crash diagnostics applied"
fi
