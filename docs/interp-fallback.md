# 阶段3b：无 barycentric 扩展的插值回退方案（设计）

> 日期：2026-11-14 · 状态：真机裁决完成，方案选型中
> 触发：DIVA FS 的 per-vertex 插值必需 VK_KHR_fragment_shader_barycentric，
> 而目标设备两个驱动均不支持。

## 真机裁决（一加 Ace 3V / Adreno 732）

| 驱动 | Vulkan | barycentric | AMD explicit vertex |
|---|---|---|---|
| 官方 Qualcomm | 1.3.128 | NO | NO |
| Turnip gen8 v5（用户指定，模拟器同款） | 1.4.363 | NO | NO |

Turnip 下设备识别为 "Adreno (TM) 735 (unknown)"（a7xx 通用路径，与用户模拟器实测一致）。
驱动二进制含扩展名字符串但设备不暴露——字符串存在≠支持，以探针为准。

## 上游 shadPS4 无现成回退（已核实源码）

vk_pipeline_cache.cpp:361：
`needs_manual_interpolation = IsFragmentShaderBarycentricSupported() && DriverID==Nvidia`
——仅 Nvidia 专有驱动启用手动插值；无扩展时 V_INTERP_MOV select 0/1 直接 ASSERT。
**结论：必须自研回退，不能靠上游配置或换驱动解决。**

## DIVA 插值用量分析（eboot 88 个 FS 样本）

- 79/88 的 FS 使用 per-vertex 插值（V_INTERP_P1/P2 + MOV select 0/1）
- 涉及 attr 索引仅 0–4：attr0=79 个 FS、attr1=37、attr2=20、attr3=8、attr4=2
- 即回退只需处理 ≤5 个属性的三顶点值，成本可控（SBO 131 FS 待同法统计）

## 候选方案

- **A. GS 展开**：几何着色器把每三角形 3 顶点的 attr 值以 flat varying 传给 FS。
  缺点：Adreno GS 走仿真、性能差；varying 带宽 ×3 起。不推荐。
- **B. VS→SSBO + FS 按 PrimitiveID 拉取（推荐）**：
  1) VS 末尾把 per-vertex 属性（≤5 个）写入 SSBO（按 VertexIndex 索引）；
  2) 可选 compute 预处理：按索引缓冲构建每三角形属性表（减少 FS 间接寻址）；
  3) FS 用 gl_PrimitiveID（Vulkan FS 核心可用）取本三角形 3 顶点值，
     替换 P10/P20/P0 读取；I/J 重心坐标同理由三角形屏幕空间位置重建或一并存储。
  优点：无扩展依赖、无 GS、确定性；缺点：管线改造 + 每片段少量 storage 读。
- C. 继续换驱动碰运气：期望值低，不作为主线。

## 实施步骤（qemu/CI 优先，真机仅最终验证）

1. SBO 131 FS 同法统计 attr 用量，确认 ≤5 属性假设
2. IR 层：新增 interp_fallback pass——识别 per-vertex GetAttribute，
   改写为 SSBO 拉取；VS 侧注入属性写出
3. 运行时：管线布局加 SSBO 绑定 + PrimitiveID 传递；PM4 索引缓冲接入
4. 验证：qemu 跑转换 + spirv-val + lavapipe 消费（lavapipe 支持 SSBO/PrimitiveID）；
   用真实 DIVA FS 做渲染对照（与有扩展路径输出比对，x86 lavapipe 可跑标准路径做基准）
5. 真机最终验证（Turnip gen8 v5 容器环境已打通）

## 风险

- I/J 重心坐标重建精度（音游画面要求亚像素正确）——需与标准路径逐像素比对
- VS 写 SSBO 的同步（VS 完成→FS 读取的屏障）——Vulkan 管线内建依赖可控
- 性能：每片段 ≤15 次 storage 读，Adreno 732 需实测帧率（阶段6 议题）

---

内容由 AI 生成
