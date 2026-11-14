# DIVA FTDX shader.sbo 容器格式（逆向分析）

> 日期：2026-11-14 · 状态：已验证（PC/PS4 双版全量提取 + 262/262 GCN→SPIR-V 转换 + spirv-val 通过）
> 样本：shader_pc.sbo（2,427,800 B）/ shader_ps4.sbo（1,649,240 B），用户自游戏 dump 提取。
> 提取器：`scripts/sbo_extract.py`（原创实现）。提取产物为游戏资产，禁止入仓库。

## 总览

SBO（Shader Binary Object）是 DIVA 的材质 shader 容器：**23 个材质 × 131 对 vertex/fragment = 262 个 shader**。
PC 版装 DXBC（DX11），PS4 版装 GCN ISA（"Shdr" 容器）。两版条目表/名字表同构。

材质列表：blinn_per_frag, blinn_per_vert, cloth_aniso, cloth_default, cloth_npr1, constant,
floor, glass_eye, hair_default, hair_npr1, item_blinn, lambert, lit_proj, puddle, silhouette,
simple, simple_reflect, skin_default, sky_default, sss_skin, stage_blinn, tights, water01。

## 文件头（32 B）

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u32 | version = 1 |
| 4 | u32 | count = 23（材质数） |
| 8 | u32 | headersize = 32 |
| 12 | u32 | pad |
| 16 | u64 | table_end（条目表结束偏移，0xD00） |
| 24 | u64 | pad |

## 条目表 [32, table_end)：变长记录

- **shader 记录**：`u32 blob1_off`，可选 `u32 blob2_off`（仅当该值 < 0x80000000），
  随后是连续的 `u32 ids[]`（每个 ≥ 0x80000000，成对出现，形如 0x80000000/0x80000001，
  语义为 technique/permutation 引用）。共 69 条（每材质 2~4 条，对应不同档位/排列）。
- **name 记录**：`u32 name_off, u32 0, u32 0, u32 0`（16 B），终止一个材质分组。共 22 条；
  首材质名由字符串区未引用名补齐（23 材质全对齐）。
- 每条 shader 记录含 1~2 个数据块，每数据块 = vertex + fragment 两个 shader。

## 名字字符串表

table_end 之后、首个数据块之前：NUL 结尾 ASCII 材质名（0xD0F 起）。

## 数据块 subheader（变长）

`u32 nsec, u32 ver=2, u32 sec_ids[nsec], u32 first_shader_size, u32 pad`，
PS4 额外跟 `u8 hash[8] + u32 f[7]`。nsec 语义两版略异，**定位直接用魔数扫描**（Shdr/DXBC）。

## PS4："Shdr" 容器

- 魔数 `Shdr` + u32 version = 0x20007（数据区内有伪魔数，必须校验 version）。
- hdr[2] & 0xFF：**1 = vertex，2 = fragment**。
- **code_off = u32@+24 + 16**（对全部 262 个容器用 prologue 校验通过）。
- 代码以 `S_MOV_B32 vcc_hi, #imm`（0xBEEB03FF + literal）开头。
- **代码终点**：首个 S_ENDPGM 可能是早退路径（分支跳到其后继续执行）。
  真实终点 = 「不小于 最大分支目标+1 的最小 S_ENDPGM 结尾」。
  分支 = SOPP op ∈ {2,4,5,6,7,8,9}，目标 = pc+1+simm16。
- 代码区之后是常量/索引数据表（非指令）。
- 指令流为 GCN ISA，shadPS4 shader_recompiler 可直接消费。
  注意 SOP1 位域：op=[15:8]、sdst=[22:16]、src0=[7:0]；
  VS 首条真实指令为 `S_SWAPPC_B64 s0,s0`（op=33）→ PS4 fetch shader 机制。

## PC：DXBC

标准 DXBC（RDEF/ISGN/OSGN/SHEX/STAT）。OSGN 含 SV_Target → fragment，否则 vertex。
与 PS4 逐位对应（同材质同档位同阶段）。

## 离线转换的 RuntimeInfo 约定（sbo_translate_test.cpp）

真机运行时这些值来自 PM4/SPI 寄存器与 guest 内存；离线验证用以下近似：

1. **FS 插值**：addr_flags 启用 persp_center + linear_center（v0/v1=smooth I/J，v2/v3=nopersp I/J）；
   inputs[0..31] 置非 default/非 flat；needs_manual_interpolation=true（Turnip 可移植路径）。
2. **FS 导出格式**：DIVA 主用 fp16 压缩导出（EXP.compr=1，123/131）。按指令流自适应：
   含压缩 EXP → ABGR_FP16，否则 ABGR_32。
3. **VS fetch shader**：真实 fetch 代码/V# 由游戏运行时生成（不在 SBO）。离线合成最小等价：
   8 个 float4 属性 → v4..v35，V# = 32_32_32_32/FLOAT/身份 swizzle；
   user_data[0:1]=fetch 代码指针，user_data[4:5]=V# 表指针。
   真机阶段改用 guest 内存真实指针后，属性布局以游戏实际状态为准。

## 验证结果（2026-11-14，x86 原生构建，g++ 14.2）

- 提取：PC 262（131v/131f）+ PS4 262（131v/131f），23 材质全对齐。
- 转换：**262/262 GCN→IR→SPIR-V 成功**（SBO_TRANSLATE_TEST_PASS）。
- 校验：**262/262 spirv-val 通过**。
- ARM64 交叉编译 + qemu + lavapipe 手写用例：CI 全绿（run 43576113866，commit 8c4ac4f）。
- Vulkan 消费（`runtime/test_sbo_vulkan.cpp`，lavapipe）：**驱动侧崩溃**，未通过——
  崩在 libvulkan_lvp.so 编译真实 shader 时（手写用例同管线正常，属 lavapipe 对复杂真实
  shader 的能力限制）。另 DIVA FS 的 1372 条 V_INTERP_MOV 全部 select=P10/P20（per-vertex
  插值），SPIR-V 必需 **VK_KHR_fragment_shader_barycentric**，lavapipe 无此扩展。
  **Turnip/Adreno 732 是否支持该扩展 = 真机必查项**；不支持则需自研插值回退。

---

内容由 AI 生成
