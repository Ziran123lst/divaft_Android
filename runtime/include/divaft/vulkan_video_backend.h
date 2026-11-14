// divaft_Android — Vulkan 视频后端（阶段3：shaders→Vulkan 消费端，headless 最小实现）
// 把 shader_recompiler 产出的 SPIR-V 创建为 VkShaderModule→管线并执行。
// 当前聚焦计算管线路径（重编译器现产 compute shader）；图形/上屏（Turnip/KGSL + Android surface）后续接入。
// 验证可在无 GPU 环境用软件 Vulkan（lavapipe/SwiftShader）实跑。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/video_backend.h"
#include <cstdint>
#include <cstddef>
#include <vector>

// 前置声明 Vulkan 句柄（避免头文件强依赖 vulkan.h，实现里再含）。
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkShaderModule_T;
struct VkPipelineLayout_T;
struct VkPipeline_T;
struct VkCommandPool_T;
struct VkDescriptorSetLayout_T;
struct VkDescriptorPool_T;

namespace divaft {

class VulkanVideoBackend final : public IVideoBackend {
public:
  VulkanVideoBackend();
  ~VulkanVideoBackend() override;

  // IVideoBackend
  bool Init(uint32_t width, uint32_t height) override;
  void SetBufferAttribute(uint32_t index, const VideoBufferAttr& attr) override;
  void RegisterBuffer(uint32_t index, uint64_t gpu_addr, uint32_t pitch) override;
  void Flip(uint32_t index, int64_t flip_arg) override;
  void WaitVblank() override;
  void SubmitCommandBuffer(const uint32_t* cmdbuf, size_t dwords) override;
  void DrawIndex(uint32_t index_count, uint64_t index_base, uint32_t initiator) override;
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  void SetRegister(uint32_t reg, uint32_t value, bool is_sh_reg) override;
  const char* name() const override { return "vulkan"; }

  // shaders→Vulkan 核心：把 SPIR-V 创建为计算管线。成功返回 true。
  bool CreateComputePipelineFromSpirv(const uint32_t* spirv, size_t word_count);
  // 图形管线离屏渲染：用 vertex+fragment SPIR-V 建图形管线，画 3 顶点，读回 RGBA 像素。
  // 顶点输入为空（适配 gl_VertexIndex / 常量导出 shader）。成功返回 true 且 out_rgba 含 w*h*4 字节。
  bool RenderGraphicsOffscreen(uint32_t w, uint32_t h, const uint32_t* vert, size_t vert_words,
                               const uint32_t* frag, size_t frag_words,
                               std::vector<uint8_t>& out_rgba);
  // 仅验证 vertex+fragment SPIR-V 能建成图形管线（不渲染）。
  bool CreateGraphicsPipelineFromSpirv(const uint32_t* vert, size_t vert_words,
                                       const uint32_t* frag, size_t frag_words);
  // 是否已就绪（instance/device/queue 建立）。
  bool ready() const { return ready_; }
  // 最近一次错误描述（诊断）。
  const char* last_error() const { return last_error_; }

private:
  bool CreateInstanceAndDevice();
  void SetLastError(const char* msg);

  bool ready_ = false;
  const char* last_error_ = "";
  uint32_t width_ = 0, height_ = 0;

  VkInstance_T* instance_ = nullptr;
  VkPhysicalDevice_T* physical_ = nullptr;
  VkDevice_T* device_ = nullptr;
  VkQueue_T* queue_ = nullptr;
  uint32_t queue_family_ = 0;
  VkCommandPool_T* cmd_pool_ = nullptr;

  VkShaderModule_T* shader_module_ = nullptr;
  VkPipelineLayout_T* pipeline_layout_ = nullptr;
  VkPipeline_T* pipeline_ = nullptr;
  bool has_pipeline_ = false;

  uint64_t flips_ = 0, dispatches_ = 0;
};

}  // namespace divaft