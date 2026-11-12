# divaft_Android

《初音未来 歌姬计划 Future Tone DX》（PS4, CUSA08053）的 **Android/ARM64 原生移植**。

> 原创移植运行时实现。FEX（CPU 翻译）与 shadPS4（渲染器/HLE）作为依赖引入。
> 不含任何游戏文件——需自备解密 dump。

## 架构

```
DIVA 游戏代码 (x86-64)
   │ FEX 翻译 → 原生 ARM64 执行
divaft_runtime（原创）
   guest_bridge · diva_loader · hle_wire · audio_out · input_pad
   │ shadPS4 video_core + HLE
Turnip Vulkan → Adreno
```

- **原创**：移植运行时（桥接/加载器/HLE 接线/音频/输入）+ 构建 + CI + App 壳。
- **依赖**（submodule）：FEX、shadPS4。
- **原生策略**：HLE/渲染/音频/输入全原生；游戏代码 FEX JIT（翻译后即原生执行），
  AOT 静态重编译作为渐进优化目标。详见 [docs/nativeness.md](docs/nativeness.md)。

## 当前状态（阶段1 地基）

- [x] 原创 DIVA 加载器（SELF/ELF 解析+重定位），已用真实 eboot 验证
      （entry 0x779500 / 198.8MB 映像 / 19071 重定位 / 39 模块）
- [x] HLE 接线框架 + 643 函数契约（`runtime/data/contract.tsv`）
- [x] guest 桥接（veneer 机制）、输入（slide 合成）、音频接口
- [ ] FEX/shadPS4 submodule 接入（阶段2）
- [ ] FEX FP bug 验证（mulsd/addpd，阶段2）
- [ ] DIVA 启动路径 / 音频 / 渲染（阶段3-4）

## 构建（本机验证加载器）

```bash
cmake -B build && cmake --build build
./build/divaft_load /path/to/eboot.elf runtime/data/contract.tsv
```

Android 交叉编译（阶段2+）：`-DDIVAFT_ANDROID=ON` + NDK + FEX/shadPS4 submodule。

## 文档

- [docs/architecture.md](docs/architecture.md) — 架构设计
- [docs/nativeness.md](docs/nativeness.md) — 原生策略与 AOT
- [docs/diva-analysis.md](docs/diva-analysis.md) — DIVA 逆向分析（导入/数据布局/音频）

## 许可

GPL-2.0-or-later（与 FEX/shadPS4 生态一致）。游戏资产版权归 SEGA / Crypton Future Media。

---

内容由 AI 生成
