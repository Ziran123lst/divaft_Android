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
