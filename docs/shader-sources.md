# DIVA FTDX PS4 shader 来源全景与转换状态

> 日期：2026-11-14 · 状态：**446/446 真实 PS4 shader 全部 GCN→SPIR-V 转换成功**
> 结论依据：eboot 字符串/容器/段/引用四重验证 + 用户确认（FARC 为街机版）。

## 来源清单

| 来源 | 数量 | 状态 | 说明 |
|---|---|---|---|
| shader_ps4.sbo | 262（131 VS + 131 FS，23 材质） | ✅ 262/262 转换 + spirv-val | eboot 唯一字面路径 `rom/shader_ps4.sbo` @0x88AF52 |
| eboot.bin 内嵌 Shdr 容器 | 186 容器 → 184 唯一 shader（87 VS + 88 FS + 9 compute） | ✅ 184/184 转换 | 工具/后处理类（tone_map、volume_light、粒子等，见 eboot 效果名注册表 @0x88AE8A） |
| shader.farc / shader_cg.farc | — | ⛔ 不需要 | **用户确认：街机版着色器**，PS4 移植排除 |
| shader_pc.sbo | — | ⛔ 不需要 | DXBC，eboot 无引用，跨平台残留 |
| fetch shader | 运行时生成 | 真机阶段处理 | 引擎按输入语义动态生成，非文件；离线用合成等价（8×float4） |

## eboot 内嵌 shader 的证据链（回应"eboot 无 shader"质疑）

1. 容器：186 个 version=0x20007 校验通过的 Shdr 容器（与 SBO 同格式）。
2. 段：全部落在 PT_LOAD 可加载段（file [0x4000,0xAFCC48) → vaddr 0x0）。
3. 指令：V_INTERP_P1/P2 插值对、S_SWAPPC fetch、EXP+ENDPGM 结构齐全。
4. 行为：184/184 通过 shadPS4 完整翻译管线。
5. 引用：64 位指针 @0xB14958、32 位 VA 引用 @0x6A9DB2/0x37F979。
6. eboot 内嵌 shader 二进制校验代码（Header+code+Footer，libSceShaderBinary 风格）@0x8DC1F8。

## 提取/转换工具

- `scripts/sbo_extract.py` — SBO 容器提取（变长条目表、分支目标定代码终点）
- `scripts/eboot_shaders_extract.py` — eboot Shdr 全量提取（自洽终点算法：
  第一个覆盖其内部全部分支目标的 S_ENDPGM；避免数据区伪分支）
- `shader_build/sbo_translate_test.cpp` — 批量转换（子进程隔离；FS 插值离线配置；
  fp16 压缩导出自适应；**深度导出 target=8 → z_export_format=R_32/mrtz_mask**；
  VS fetch shader 合成；compute 支持）

## 离线 RuntimeInfo 约定（真机需换真实状态）

FS 插值 addr_flags/inputs、导出格式、fetch 布局均为离线近似；
真机运行时来自 PM4/SPI 寄存器与 guest 内存，届时按实际状态填充。
FS per-vertex 插值必需 VK_KHR_fragment_shader_barycentric —— Turnip/Adreno732 支持为真机必查项。

## 本地持久化布局（防环境重置）

- 持久（workspace，ossfs）：源码 staging `divaft-build/`、产物库 `divaft-persist/`
  （artifacts.tar.gz = SPIR-V+日志+二进制；shadps4-src-cache.tar.gz = 118MB 依赖源码缓存）、
  SBO 原件 `sbo_shaders/`、eboot 提取 `eboot_shaders_full/`。
- 临时（/root/divaft-work，overlay）：仅构建中间物。ossfs 不支持随机写，
  编译/git 必须在临时盘；产物经 Python 分块顺序写回持久区。
- 重置后一键恢复：`GH_TOKEN=<token> bash divaft-build/scripts/rebuild_local.sh`
  （装工具链→克隆→缓存恢复依赖→构建→提取→转换→产物持久化）。

---

内容由 AI 生成
