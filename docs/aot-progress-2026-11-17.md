# AOT 路线进度快照（2026-11-17）

总完成度粗估 78%。

| 阶段 | 状态 |
|---|---|
| 阶段2 CPU | 完成：DIVA 100% 原生 AOT（57133 单元 0 不可翻译），游戏本体原生执行到渲染阶段 |
| 阶段3 图形 | 管线跑通：qemu+lavapipe 全 GPU 模式，327 HLE 符号，游戏提交真实 PM4 命令；shader 446/446 SPIR-V 已备（见 shader_build/） |
| 阶段4 音频 | 最小 Ngs2 HLE 通（11 函数）；完整合成待做 |
| HLE 导入 | 100% 解析（字典 709 + 别名 404 + 自写模块） |
| App 壳 | 待做（Bloodborne-Android launcher 适配或 Termux 直跑） |

## 当前停点
eboot unit+0x555370（std::string 析构/SSO 清理路径）空指针解引用 address 0x4，
发生在 sceGnmInsertPushMarker/PopMarker（shadPS4 HLE 正常处理）之后。
0 桩状态下仍崩 → 非缺函数，是返回值语义或初始化状态问题。
下一步：AOT 故障处理器加 guest 寄存器 dump（GuestState.gregs+rip）定位空值来源。

## 验证命令
见 aot/README.md 与 aot/scripts/run_qemu_gpu.sh。
真机 Turnip（gen8 v5）为性能优化项，非阻塞——图形已可 qemu 自主调试。

## 历史路线
原 FEX-JIT 路线（runtime/）推进至 locale/iostream ABI 墙后转 AOT；
其成果（加载器/HLE 契约 643 条/shader 全集提取与转换/插值回退）仍被 AOT 路线复用
（contract.tsv 用于 NID 字典生成）。

## 崩溃根因更新（2026-11-17 12:00）
崩溃对象 = "rom/shader_ps4.sbo" 归档访问器（懒加载，构造器故意置空 [obj]）。
rndpass_pre_proc 单例初始化读其 getter 时 [obj] 仍空 → 崩溃。
运行中游戏未尝试 open rom/shader_ps4.sbo，且 ram/MiscData/CalibData/PrimaryData 全 not found
→ 游戏数据 dump 缺 rom/ 与 ram/ 目录。已补 rom/shader_ps4.sbo（来自 device-pack）仍崩，
说明 shader 归档加载初始化步骤未被触发（上游 HLE 或数据依赖）。
待办：用户补完整 dump（rom/shader_ps4.sbo + ram/PrimaryData 等）；追踪归档加载触发点。

## 重大突破（2026-11-17 13:30）：shader 归档路径修正，boot 推进到渲染线程
前崩溃根因实为**路径放错**：游戏找的是 rom_ps4_fix/rom/shader_ps4.sbo 与
rom_ps4_fix_tw/rom/shader_ps4.sbo（社区修复补丁目录，BB_FILE_TRACE 实证），不是 rom/。
把 shader_ps4.sbo 放到 rom_ps4_fix{,_tw}/rom/ 后：shader 归档被反复读取、5 个 renderer_worker
线程 + TrophyControlThread 创建、boot 从 216 行推进到 379 行，越过 rndpass_pre_proc 崩溃。
新崩点：host glibc libc.so.6+0x8f228（sem_trywait 附近，信号量操作），调用者 libc+0x828b8，
渲染线程经 libc Fios IO 调入。疑同步原语 HLE 把 guest 指针当 host sem_t*。
诊断增强：崩溃报告 dladdr 符号化 fault/caller 模块+偏移；栈/帧探针缩范围防守护页嵌套 fault。
仍缺数据：rom_ps4_fix{,_tw}/rom/game_db.txt + rob/*.farc（游戏数据库清单/动作库），ram/ 目录。

## 崩溃链彻底查清（2026-11-17 14:00）：根因=游戏数据不完整
新崩点实锤为 glibc **strlen**（libc+0xa4508：ld1{v0.16b},[x1] + cmeq #0，NEON 找 null 字节）
与 **vfprintf**（libc+0x561f0）对 NULL 字符串指针解引用。渲染线程经 Fios IO 调入。
机制：game_db.txt（游戏数据库）+ rob/*.farc（动作库）缺失 → 资源名/路径查询返回 NULL →
strlen(NULL)/printf(NULL) 崩溃。shader 归档已加载（路径修正后），但下游数据库缺失。
**结论：代码侧已推到数据依赖边界，剩余阻塞=完整游戏数据。**
需用户补：rom_ps4_fix/ 与 rom_ps4_fix_tw/ 整个目录（含 rom/game_db.txt + rom/rob/*.farc
+ rom/shader_ps4.sbo），以及 ram/（MiscData/CalibData/CloseData/PrimaryData）。
诊断工具链（寄存器/内存探针/栈回溯/HLE追踪/文件追踪/模块符号化/host backtrace）全入库。

## 音频重大进展（2026-11-17 14:50）：真实 Ngs2 DSP 合成器集成
移植 shadPS4 完整 Ngs2（3217 行，自包含无 audio_core 依赖，纯软件合成渲染进 guest 缓冲）
进 gpu/shadps4/core/libraries/ngs2，经 bbgpu_register_kernel 注册（cpu-only 也生效），
替换最小静音桩。aot/scripts/port_ngs2.sh 可复现。
实证：Lib.Ngs2 sceNgs2VoiceControl 被调 27 万次（游戏主动控制语音）；
AudioOut 输出端口已打开（port 0/1 2ch、port 17 1ch，s16 512帧@48kHz，runtime_audio.c SDL3/FIFO）。
音频输出后端：qemu  headless BB_AUDIO=none 写 FIFO/BB_AUDIO_DUMP 可验 PCM；真机需 AAudio。
注：shadPS4 的 sceNgs2VoiceControl 本身是 stub（返回 OK 未应用参数），完整语音参数需后续补。
实际出声需游戏越过缺数据崩溃（game_db.txt/rob farc）进入渲染循环。

## 音频诚实纠正（2026-11-17 15:00）
前条"真实 Ngs2 DSP 合成"表述**高估了**，纠正：shadPS4 的 Ngs2 本身是**管理骨架**——
SystemRender 仅校验缓冲后 memset 0（静音），sampler/pan/eq/reverb/mastering/submixer 全是
10 行空占位，**无任何真实 DSP**。移植它得到的是更完善的句柄/rack/voice 生命周期管理+校验，
仍渲染静音。
DIVA 真实音频引擎是 **FMOD**（静态链入 eboot，无 libSceFmod 导入，原生 AOT 执行），
runtime_audio.c 注释证实"FMOD 输出线程"调 sceAudioOutOutput。故主音频通路是
FMOD(原生)→sceAudioOutOutput→runtime_audio.c(SDL3/FIFO/AAudio)，Ngs2 或仅系统音后端。
AudioOut 输出端口已打开（port 0/1/17）。真实出声需游戏越过缺数据崩溃进入 FMOD 输出循环。
音频进度修正：输出基础设施在位（AudioOut+FMOD原生），Ngs2 骨架（FMOD 主输出或绕过它），
真实声音验证仍卡游戏数据。阶段4 约 50%（非 60%）。
