# FTDX（初音未来 歌姬计划 Future Tone DX）PS4→Android 原生移植 · 完整移交文档

> 面向接手的 AI / 工程师。本文档力求自包含：目标、架构、已验证成果、当前精确停点、全部已测试假设与结果、关键地址/符号/路径/构建配方、环境限制、以及优先级排序的下一步行动。读完应能直接继续，而不必重新摸索。
> 最后更新：2026-11-20 11:14（Asia/Shanghai）。

---

## 0. 一句话现状

基础设施（CPU 全量 AOT、GPU Vulkan、音频、呈现、输入）**已全部打通并取证**；游戏 boot 能到 `Output resolution 1920x1080 (live)` 并 SHOW 过若干帧，但**尚未「完全正常启动」（持续画面+声音）**。最后阻塞是一条崩溃/死锁链，最新定位到 **渲染上下文初始化缺陷 + libc 堆完整性检查 abort**。旁路（bypass）只能治标，已验证治本需要正确的渲染上下文/堆初始化。

---

## 1. 目标与约束

- 游戏：《初音未来 歌姬计划 Future Tone DX》PS4，TitleID `CUSA08053`。
- 目标设备：一加 Ace 3V（PJF110），骁龙 7+ Gen 3（SM7675），Adreno 732（a7xx），Vulkan 1.3，12GB RAM，Android 16 / ColorOS 16。
- 路线：**AOT 静态重编译**（bb-recomp 工具 + Bloodborne-Android 运行时适配），零运行时 JIT。libc.prx / libSceFios2.prx 离线链接进镜像，绕过 C++ locale/iostream ABI 墙。
- 用户约束：
  - 游戏资产（~20GB farc）**不上传云端**，留用户手机/PC，运行时读取。云端游戏目录只有 db+shader（见 §6）。
  - 性能要最好；无现成工具时自选最优解。
  - **修到完全正常启动才汇报**，不要中间「完成」汇报，不谎报。
  - 交付二进制一律改名 `*.txt`，用户下载后去 `.txt` 后缀；小改动走「数据目录覆盖件」。
  - 每次修正提交 GitHub。

## 2. 仓库与持久化

- 主仓库（私有）：`github.com/Ziran123lst/divaft_Android`，分支 main。
  - 推送：`git push "https://x-access-token:<TOKEN>@github.com/Ziran123lst/divaft_Android.git" HEAD:main`（TOKEN 见用户凭据，勿外泄）。
  - 工程在 `aot/{patches,scripts,docs,launcher,overlay}` + README + 进度文档。
- 上游参考：`ducvd89/Bloodborne-Android`（运行时）、`ducvd89/Bloodborne-android-rebuild-tool`（bb-recomp）、`shadps4-emu/shadPS4`（HLE 参考）。
- 云盘持久区：`/root/userdata/workspace/divaft-persist/`
  - `diva-aot/{libbbaot.so, libbbaot-glibc.so, libbbaot.json, boot-linked.bin(211904412B), content.bin(28B)}`
  - `bb-runtime/{bb-probe, libbbcpu.so, libbbgpu.so(147636712B bionic), libc++_shared.so}`
  - `bb-runtime-glibc/{bb-probe, libbbcpu.so, libbbgpu.so(100634168B)}`
  - `LLVM-23.1.3-Linux-X64.tar.zst`、`ndk-r30-linux.zip`、`shadps4-src-cache.tar.gz`、`restore_aot.sh`、`game-rom-db/{game_db.txt,pv_db.txt}`、`turnip-pack/`、`libdivadiag.so`、`DIVA-Launcher.apk`
- 工作区源码（持久）：`/root/userdata/workspace/divaft-src/{probe.c, runtime_mutex.c, runtime_thread.c, runtime_kernel.c, runtime_sema.c, runtime_memory.c, runtime_audio.c, runtime_pad.c, ...}`
- APK 工作区：`/root/userdata/workspace/divaft-apk/{DIVA-Launcher.apk, libbbprobe.so.txt, libdivadiag.so.txt, src/...}`
- qemu 恢复脚本：`/root/userdata/workspace/divaft-qemu/resume.sh`（一键重建 qemu 环境并跑）。
- **环境极频繁重置**（约每 15–40 分钟）：`/root` 全清（divaft-work、diva-test、ndk、apkbuild、apt 包）。持久区仅 `/root/userdata/workspace/...`。所有 /root 下构建都需 resume.sh 重建。

## 3. 架构与已验证成果

- AOT：bb-recomp v1.3.0（ABI2）。DIVA 全量 **57133 units / 27490 functions，0 不可翻译**。镜像 `boot-linked.bin` 格式 BBPROBE5，header 0xbe270，image base 0x800104000，libc.prx @+0xc6d0000，Fios2 @+0xc7d0000。
- 导入 100% 解析：`import_names_diva.inc`(709) + `diva_nid_alias.inc`(404) + 18 strcmp 变体 + savedata CUSA08053 + BB_IMPORT_STUBS。
- CPU：qemu-aarch64 glibc 与真机 bionic 均原生执行游戏本体（100% AOT）。
- GPU：真机 Turnip（Android 原生 R2，`libvulkan_freedreno.so` 8362496B，HMI 入口）直载成功；qemu 用 lavapipe。Vulkan 渲染+4096B 回读 PASS（`--vulkan-only`）。
- 呈现：上游 bbpresent（AHB swapchain + ASurfaceControl，SOCK_SEQPACKET+SCM_RIGHTS），全屏独立 DisplayActivity。真机 SHOW 过帧（最多 ~5-60 帧不等）。
- 音频：runtime 写 FIFO（s16 stereo 48kHz），Java AudioBridge 读 FIFO→AudioTrack。FIFO 已改 O_RDWR|O_NONBLOCK 防 pipe_write 死锁。
- 输入：runtime_pad 读 BB_TOUCH_FILE，DisplayActivity 四象限触屏。
- 已修复并验证的具体崩溃：HOME/XDG env 缺失(strlen NULL)、Ngs2 句柄(null voice handle)、音频 FIFO 死锁、COND 刷屏 pipe_write、渲染 NULL/垃圾基址崩溃族。

## 4. 当前精确停点与崩溃链（最重要）

真机与 qemu 的最新日志收敛到同一条链：

1. **渲染上下文 NULL/垃圾/未映射**：renderer_worker 调参数设置器
   - `0x48c2e0`: `mov rax,[rdi+0xf0]`（读参数池指针链）
   - `0x48c250`: `mov byte [rdi+0x850],sil`（写参数）
   - `0x6598a0`/`0x568170`/`0x65b1e0`/`0x472530` 同族。
   - 分发器在 guest `0x59d668: mov r14,[rbx+0x28]` → `call 0x64b6d0`(取索引) → `call 0x54a550/0x48c2e0`。`[rbx+0x28]` = 渲染任务对象里的「谱面/参数上下文」指针，**初始化期未被正确赋值**（NULL / 代码字节垃圾 / 中段未映射堆 / host 范围指针，随配置变化）。
2. **libc 堆完整性检查 abort**：`[SceLibc] A heap error is detected.` → `bbr_trap` → libc.prx+0x5d7d4 = guest `0xc72d7d4`（`int 0x44`+`mov edx,0xcccccccc`+`mov ecx,0xa002000b`+`ud2`）= PS4 libc abort/assert。
3. **启动期 cond/信号量循环死锁**（breaker 关时）：主线程作为工作生产者等待完成，worker 池等待工作，循环等待；breaker 开则提前广播破坏上下文。

### 4.1 旁路（probe.c `nullparam_skip`）当前逻辑与教训
- 位置：`divaft-src/probe.c`，在 `fault()` 顶部先于 guest_cpu_handle_fault 调用。
- 演进与结果：
  - 低/高基址旁路（base<0x100000 或 >0x100000000000）→ 清除 NULL/垃圾崩溃，但中段未映射堆写仍崩。
  - bypass-all（所有 guest load/store）→ 清除崩溃但**把指针加载归零**→游戏 free/realloc 错指针→**堆损坏→heap error abort**。
  - store-only 广谱 + load 低/高 → 仍 heap error（偷走了写保护页的堆元数据写）。
  - **最新（当前源码）：MAPERR-only**（`if (info->si_code != SEGV_MAPERR) return 0;`，ACCERR 交还 UMA/page-tracking）→ **heap error=0**（堆元数据写不再丢失）。剩余 `0x48c250` store 到 host 范围地址 `0x7fc5ffc012b8`（ctx 被存入 host 指针=类型混淆），仍 fault。
- 教训：旁路会掩盖/诱发堆损坏；**治本=正确初始化渲染上下文与堆**，不是旁路。

### 4.2 已测试且无效的假设（不要重复）
- 递归互斥锁（PS4 默认 recursive）：无效。
- 轮询 cond_wait 50ms（失唤醒假设）：无效。
- breaker 开/关、renderer_worker 延迟 0/5/10s：均不能同时解决死锁+上下文。
- BB_TOGGLE_FILE 全关并行优化（串行路径）：无效。
- wait-profile（BB_WAIT_PROFILE/BB_WAIT_LOG）：guest HLE 无记录→主堵点在 libbbgpu 宿主层或堆；dumper 线程曾致早期 stall（已移除创建）。
- bbr_trap abort-caller dump：栈已 unwind，无文本返回地址，无增量。

## 5. 关键地址/符号/常量

- entry 0x779500；旧崩溃链 0x1f96f0→0x1f2e10→0x1f23c0→0x5580e0→0x551e64→0x555370（rndpass_pre_proc，读 rom/shader_ps4.sbo）。
- 渲染族：0x48c2e0/0x48c250/0x6598a0/0x568170/0x65b1e0/0x472530；分发 0x59d668/0x59d7xx/0x5681xx；取索引 0x64b6d0。
- abort：guest 0xc72d7d4（libc.prx+0x5d7d4）。
- libbbgpu bionic 崩溃：+0xbc907c/+0xbccc2c（_GLOBAL__sub_I_path_util.cpp，getenv HOME strlen NULL）。
- NID 例：959qrazPIrg#m#I(idx669)、73WQ4Jj0nJI#K#L(sceScreenShotSetOverlayImageWithOrigin)、MwmHz8pAdAo#P#Q(sceNgs2RackGetVoiceHandle)、pgFAiLR5qT4#P#Q(sceNgs2SystemQueryBufferSize)。
- GuestRegs={gpr[16],rip}，RSP=gpr[4]。text 范围 0x800000000..+0x50d96dc。
- PadData 120B，touches@60，timestamp@80；BTN_CROSS=0x4000 等。
- MIX_FRAMES 512；FIFO s16 stereo 48k。

## 6. 数据限制（qemu 无法完全启动的原因）

- 云端游戏目录 `/root/userdata/workspace/projects/120f93b25707450895073edba6a4d4e8/game/CUSA08053/` 只有：eboot.bin/eboot.elf、ram/(空文件)、rom/{game_db.txt,pv_db.txt,shader_pc.sbo,shader_ps4.sbo}、rom_ps4_fix*/rom/、sce_module/、sce_sys/、sce_discmap*.plt。**farc 资产包=0**。
- 因此 qemu 资产加载不完成→渲染上下文用假数据→完整性检查 abort。**qemu 只能验证到崩溃族/基础设施，不能验证完全启动**。
- 真机有全量数据（`/storage/emulated/0/projectdivafuture/CUSA08053`），是完全启动的可行验证目标。
- 若要在 qemu 验证完全启动：需用户上传 farc 资产到云侧游戏目录。

## 7. 构建配方（重置后）

- 一键：`bash /root/userdata/workspace/divaft-qemu/resume.sh`（clone bb-android/bb-recomp/repo、清华 sysroot 含 dev、NDK、zydis 共享 cmake、atrac9、glibc probe、diva-test、Xvfb+lavapipe、跑 qemu）。
- glibc probe 链接：`clang --target=aarch64-linux-gnu --sysroot=$SR -std=c11 -O2 -g -pthread -DBB_NO_SDL -I. -Isrc src/probe.c src/runtime*.c src/vulkan_smoke.c out/g/libatrac9.a -Lout/g/lib -lbbgpu -lbbcpu -L$SR/usr/lib -lZydis -lZycore $L/libfmt.so.10 $L/libxxhash.so.0 $L/libavformat.so.61 ... -lSDL3(若SDL) -lm -lpthread -ldl -lstdc++ -Wl,-rpath,'$ORIGIN' -pie`。
- bionic probe：`aarch64-linux-android30-clang -std=c11 -O2 -g -pthread -I. -Isrc src/probe.c src/runtime*.c src/vulkan_smoke.c out/bN/libatrac9.a -Lout/bN/lib -lbbgpu -lbbcpu -lm -Wl,-rpath,'$ORIGIN/../lib' -rdynamic -pie`。
- libbbcpu 手动重编：`clang++ --target=aarch64-linux-gnu --sysroot=$SR -std=c++20 -O2 -g -fPIC -shared -I. -Isrc -Icpu cpu/guest_cpu_aot.cpp cpu/guest_cpu_native.cpp cpu/host_call_arm64.S -o libbbcpu.so -lpthread -ldl`。
- libbbgpu 完整重编需 `aot/scripts/diva_arm64_build.sh`（依赖 LLVM23.1.3+Boost1.92+VMA+magic_enum+robin-map+miniz+vk-prefix，依赖树在 .local-deps，**重置后不保留**，是当前最大工程障碍）。
- zydis/zycore：cmake -DZY*_BUILD_SHARED_LIB=ON 装到 $SR/usr（tar 只存符号链接，需重编）。
- qemu 跑：`qemu-aarch64 -L $SR ./bb-probe boot-linked.bin --app0 "$G" --content-profile content.bin --timeout N`，LD_LIBRARY_PATH 含 syslibs+out/g/lib+$SR/usr/lib...，VK_ICD_FILENAMES=lvp.json，SDL_VIDEODRIVER=x11 DISPLAY=:99。
- 截屏验证：Xvfb :99 + `ffmpeg -f x11grab -video_size 1920x1080 -i :99 -frames:v 1 out.png`。

## 8. 环境/工具坑

- ossfs 只顺序追加写；大文件用 python 分块拷贝；链接器输出不能直接写 ossfs（先 /root 再拷）。
- edit/write 偶发静默丢写→必须读回校验。
- `pkill -f <pattern>` 会自匹配当前 shell 自杀→用 `for p in $(pgrep qemu-aarch64); do kill $p; done`。
- 长命令>2-3min 触发网关 fetch failed→nohup+轮询。
- Debian 用清华镜像（aliyun 限流）。
- runtime 开关多为 **getenv 存在性**判断：setenv "0" 无效，须 unsetenv。
- read/edit 工具仅限 workspace；/root 用 bash。

## 9. 交付物（当前）

- `divaft-apk/libbbprobe.so.txt`（最新 ~1354440，MAPERR-only 旁路+heap 修复）、`libdivadiag.so.txt`(51376)、`DIVA-Launcher.apk`、`libvulkan_freedreno.so`(Turnip R2)。
- 覆盖方式：去 .txt 覆盖 `Download/diva-data` 同名文件，点 4 开始。

## 10. 给接手 AI 的优先级行动

1. **治本渲染上下文**：逆向渲染任务对象构造工厂，找 `[rbx+0x28]` 赋值点（虚分发/函数指针，静态不收敛，需运行时构造跟踪：hook 分配+字段写，或硬件断点）。让 ctx 真实初始化，而非旁路。
2. **堆 HLE**：确认 guest 堆（malloc/calloc/realloc/free、sceKernelAllocateDirectMemory 等）映射与元数据写正确；heap error 的根因是堆页未正确映射/写保护处理。修好后 heap error 应消失且不需旁路。
3. **cond/调度语义**：若仍有启动死锁，补 PS4 优先级继承/调度或修正 Fios/启动工作投递序。
4. **验证**：真机全量数据复测（可行目标）；或上传 farc 到云侧后 qemu 截屏见非黑帧+持续音频才算完全启动。
5. 稳定构建环境（不重置）以重编 libbbgpu 依赖树，是完成 1-3 的前提。

## 11. 诚实边界

- 已验证：基础设施全通、崩溃族可旁路清除、heap error 已用 MAPERR-only 修复。
- 未验证/未达成：完全正常启动（持续画面+声音）。不谎报。
- 旁路包≠修复；最终需正确初始化。
