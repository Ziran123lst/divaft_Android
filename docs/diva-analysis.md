# DIVA Future Tone DX 逆向分析（CUSA08053）

> 本项目原创分析成果，作为移植依据。对象：港版中日文 CUSA08053，APP_VER 01.01。

## 可执行文件

- eboot.bin：伪签名 SELF（PT_FAKE），10 段全未加密未压缩，可直接加载。
- 内嵌 ELF64 x86-64，ET_SCE_DYNEXEC，entry **0x779500**。
- 映像 198.8MB，11 段，**19071 重定位**（JUMP_SLOT 602 / RELATIVE 17599 / 64位 870）。
- 加载器已验证：`runtime/diva_loader.cpp`（原创）。

## 导入（HLE 契约）

- **643 个导入函数**，39 依赖模块，38 导入库。契约见 `runtime/data/contract.tsv`。
- 覆盖分析（对照 shadPS4）：
  - 399 个 shadPS4 已注册 HLE（可直接用）
  - 234 个由游戏自带 libc.prx / libSceFios2.prx 提供
  - 10 个 aerolib stub（9 内容管理 + __stack_chk_guard，PC 已证明可玩）
- 全部 PS4 系统库 100% 覆盖：GnmDriver 103、Ngs2 11、Pad 12、AudioOut 6、
  AvPlayer 15、SaveData 11、VideoOut 8 等。

## 关键库

- **Ngs2**（音频引擎，音游命脉）：shadPS4 完整 DSP 实现约 1614 行 / 11 文件，
  纯标准 C++ 零 kernel 依赖。DIVA 需 11 函数（System/Rack/Voice）。
- **AudioOut**：强制 48kHz，同步推送模型（guest 线程被后端消费节奏驱动）。
- **OGG 音频**：DIVA 不导入 Audiodec，293 个 .ogg 由游戏自解（Vorbis 软解）。
- **30 个 MP4 PV**（4.67GB）走 AvPlayer。

## 数据布局（社区修复版重打包）

- 总 29.51GB / 16371 文件。
- `rom_ps4_fix/rom`：全部游戏数据（eboot 原生含 rom_ps4_fix 路径字符串，布局保持即可）。
- `rom_ps4_fix_tw`：繁中覆盖层（103MB）。
- `rom_ps4_dlc/M001-M004`：DLC 描述符（info.txt，Shift-JIS 键值），
  含 pv/module(.obj)/item/script/sp_skp.db 五类键。DLC 内容已并入 fix rom。
- 资产：FARc 7764 个/21.5GB、TGA 1.1GB、IBL 342MB、DSC 节奏脚本 1002 个。

## 输入

- 四键音符 △○×□ + hold + slide（触摸板滑动或 L1/R1）+ chain slide。
- slide 原生读 OrbisPadData.touchData → 屏幕滑动合成触摸板轨迹（1920x942），零游戏修改。

## 目标设备

一加 Ace 3V：骁龙 7+ Gen 3（Adreno 732），12GB，Android 16，4KB 页（FEX 已实证）。
a732 Turnip 需 chip_id + 设备项（社区驱动可先用）。

---

内容由 AI 生成
