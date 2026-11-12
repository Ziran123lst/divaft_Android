# divaft_Android 架构设计

> 日期：2026-11-12 · 状态：用户已批准（"好，那你開始"）
> 目标：把《初音未来 歌姬计划 Future Tone DX》（PS4, CUSA08053）原生移植到 Android/ARM64。
> 原则：移植运行时原创实现（不照搬 gow3/uncharted），FEX/shadPS4 作为依赖引入；运行尽可能原生。

## 1. 设计决策（已确认）

- **原创范围**：移植运行时（guest 桥接 / DIVA 加载器 / HLE 接线 / 音频 / 输入）+ 构建系统 + CI + App 壳。
- **依赖引入**（submodule，不从零重写）：FEX（x86-64→ARM64 翻译）、shadPS4（video_core 渲染器 + HLE 库）。
- **不纳入**：任何游戏文件（版权）；用户自备解密 dump。
- **原生策略**：native-first + AOT 渐进（见 §4）。

## 2. 分层架构

```
DIVA 游戏代码 (x86-64, 用户 dump)
        │  FEX 翻译（依赖）→ 原生 ARM64 执行
┌───────▼────────────────────────────────┐
│ divaft_runtime（原创）                   │
│  guest_bridge  veneer 桥接 SysV↔AAPCS64  │
│  diva_loader   eboot/SELF 解析+加载       │
│  hle_wire      NID→shadPS4 HLE 接线      │
│  audio_out     Ngs2→AAudio（音游低延迟）  │
│  input_pad     触屏→OrbisPadData/slide   │
└───────┬────────────────────────────────┘
        │  shadPS4 video_core + HLE（依赖）
┌───────▼────────────────────────────────┐
│ Turnip Vulkan → Adreno 732             │
└────────────────────────────────────────┘
```

数据流：guest 代码经 FEX 执行 → 调用系统函数时跳入 guest_bridge 的 veneer →
桥接层把 SysV 参数转 AAPCS64 → 调 hle_wire 接线的 shadPS4 HLE（原生 ARM64）→
图形走 shadPS4 video_core→Vulkan，音频走 audio_out→AAudio。

## 3. 模块职责与文件

| 模块 | 文件 | 职责 |
|---|---|---|
| guest_bridge | runtime/guest_bridge.{h,cpp} | veneer 生成、guest↔host 调用桥接、线程/信号协调 |
| diva_loader | runtime/diva_loader.{h,cpp} | SELF/ELF 解析、段映射、rom_ps4_fix 布局、重定位 |
| hle_wire | runtime/hle_wire.{h,cpp} | NID 解析表、shadPS4 HLE 库注册接线 |
| audio_out | runtime/audio_out.{h,cpp} | Ngs2 输出→AAudio 低延迟、判定偏移校准 |
| input_pad | runtime/input_pad.{h,cpp} | 触屏分区→按键、滑动→触摸板轨迹合成 |

## 4. 原生策略（native-first + AOT 渐进）

- HLE / 渲染器 / 音频 / 输入：全原生 ARM64（占运行大头，音游关键路径）。
- 游戏代码：FEX JIT 翻译后已是原生 ARM64 执行（缓存摊销翻译开销）。
- AOT 渐进：运行时翻译层与执行层分离（AOT 友好）；先 FEX 跑通，再对热点/稳定路径
  静态重编译，动态部分（SMC/间接跳转）回退 JIT。完整 AOT 是研究目标非保票。

## 5. 已知风险与对策

- **FEX FP bug**（mulsd/addpd）：依赖用更新 FEX 版本先验证；原创运行时保留绕开控制权。
- **a732 Turnip**：需 chip_id + 设备项（社区驱动可先用）。
- **音频延迟**：AAudio MMAP 低延迟 + 判定偏移校准。

## 6. 分阶段路线

1. 地基：结构 + 原创运行时骨架 + CMake/NDK + submodule + CI
2. FP 验证：新 FEX 跑 phase1
3. DIVA 启动路径：加载器 + HLE 接线 → 游戏初始化
4. 音频/输入/渲染专项 → 可玩

## 7. 下一步

地基（阶段1）实施中：仓库结构、原创运行时、构建系统、CI。

---

内容由 AI 生成
