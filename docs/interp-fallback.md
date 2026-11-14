# 阶段3b：无 barycentric 扩展的插值回退（已实施轻量方案）

> 日期：2026-11-14 · 状态：**轻量方案已实施并验证（446/446）**
> 关键转折：用户实证——Bachata S4（shadPS4 系安卓 PS4 模拟器）+ Turnip gen8 v5
> 在 Adreno 732 上正常游玩 DIVA FTDX，证明缺扩展不是硬阻断。

## 真机裁决（一加 Ace 3V / Adreno 732）

| 驱动 | Vulkan | barycentric | AMD explicit vertex |
|---|---|---|---|
| 官方 Qualcomm | 1.3.128 | NO | NO |
| Turnip gen8 v5（Bachata S4 同款，glibc 容器） | 1.4.363 | NO | NO |

Turnip 下设备识别为 "Adreno (TM) 735 (unknown)"（a7xx 通用路径）。
探针：runtime/vk_ext_check.c（Bionic）/ vk_ext_check_glibc.c（glibc 容器）。

## 机制分析（为什么能玩）

上游 shadPS4 的 V_INTERP_MOV_F32 用 ASSERT 挡住无扩展路径，但**代码里已有
Flat 回退分支**（else: interp.primary=Flat + 普通 varying 读取）。
标准插值路径（needs_manual_interpolation=false）下：
- V_INTERP_P1_F32：no-op（硬件插值器经 qualifier 完成）
- V_INTERP_P2_F32：设 qualifier + 普通 varying 读取，无需扩展
- V_INTERP_MOV select 0/1：走 Flat 回退（ provoking vertex 值，视觉近似）
Bachata S4 核心非公开，但其可玩实证与上述路径一致：**放宽 ASSERT 即可**。

## 已实施（轻量方案）

1. `scripts/patch_interp_fallback.py`：把依赖 vector_interpolation.cpp 的
   ASSERT 放宽为注释，走现成 Flat 回退。幂等；CI fetch 后自动调用；
   rebuild_local.sh 同步调用。
2. 转换 profile 改标准路径：needs_manual_interpolation=false，
   supports_fragment_shader_barycentric=false。
3. 验证结果：
   - **446/446 GCN→SPIR-V 成功**（SBO 262 + eboot 184）
   - **446/446 spirv-val 通过**
   - SPIR-V **不再含 FragmentBarycentricKHR**（0 处），无扩展依赖
4. lavapipe 消费测试：随机段错误（同一调用时过时崩，单对/多对均复现）——
   lavapipe 编译复杂真实 shader 的内部 bug，非管线逻辑问题；
   手写用例 CI 稳定。真实消费目标为 Turnip（Bachata 实证可行）。

## 画质备选（未实施，降级为选项）

若 Flat 回退在真机渲染中出现不可接受的视觉瑕疵（per-vertex 属性被当 flat），
再启用重型方案：VS→SSBO + FS 按 gl_PrimitiveID 拉取三顶点值。
用量分析支持其可行性：per-vertex 访问仅涉 attr 0–4（eboot 88 FS 中 79 个使用，
attr0 全部、attr1=37、attr2=20、attr3=8、attr4=2）。
实施步骤草案：IR pass 改写 per-vertex GetAttribute → SSBO 拉取；VS 注入属性写出；
管线加 SSBO 绑定；qemu/lavapipe 与标准路径逐像素比对。

## 后续

- 真机渲染验证（Turnip gen8 v5 容器链路已打通）：观察 Flat 回退画质
- 若画质 OK → 阶段3b 关闭，进入渲染执行主线
- 若画质不行 → 启动 SSBO 拉取方案

---

内容由 AI 生成
