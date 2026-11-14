// divaft_Android — Vulkan 视频后端实现（headless 最小，计算管线路径）。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/vulkan_video_backend.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>

namespace divaft {

VulkanVideoBackend::VulkanVideoBackend() = default;

VulkanVideoBackend::~VulkanVideoBackend() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (shader_module_) vkDestroyShaderModule(device_, shader_module_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VulkanVideoBackend::SetLastError(const char* msg) {
  last_error_ = msg;
  std::fprintf(stderr, "[vulkan-backend] ERROR: %s\n", msg);
}

bool VulkanVideoBackend::CreateInstanceAndDevice() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "divaft";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  // headless：不需要 surface 扩展。
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    SetLastError("vkCreateInstance failed");
    return false;
  }
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  if (count == 0) {
    SetLastError("no Vulkan physical device (need lavapipe/SwiftShader/Turnip)");
    return false;
  }
  std::vector<VkPhysicalDevice> devs(count);
  vkEnumeratePhysicalDevices(instance_, &count, devs.data());
  physical_ = devs[0];

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qcount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qcount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qcount, qprops.data());
  bool found = false;
  for (uint32_t i = 0; i < qcount; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queue_family_ = i;
      found = true;
      break;
    }
  }
  if (!found) {
    SetLastError("no compute queue family");
    return false;
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
    SetLastError("vkCreateDevice failed");
    return false;
  }
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.queueFamilyIndex = queue_family_;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(device_, &cpci, nullptr, &cmd_pool_) != VK_SUCCESS) {
    SetLastError("vkCreateCommandPool failed");
    return false;
  }
  return true;
}

bool VulkanVideoBackend::Init(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  if (!CreateInstanceAndDevice()) {
    return false;
  }
  ready_ = true;
  std::fprintf(stderr, "[vulkan-backend] ready (headless, compute queue family %u)\n", queue_family_);
  return true;
}

bool VulkanVideoBackend::CreateComputePipelineFromSpirv(const uint32_t* spirv, size_t word_count) {
  if (!ready_) {
    SetLastError("backend not initialized");
    return false;
  }
  if (has_pipeline_) {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = nullptr;
    has_pipeline_ = false;
  }
  if (shader_module_) {
    vkDestroyShaderModule(device_, shader_module_, nullptr);
    shader_module_ = nullptr;
  }
  VkShaderModuleCreateInfo smci{};
  smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smci.codeSize = word_count * sizeof(uint32_t);
  smci.pCode = spirv;
  if (vkCreateShaderModule(device_, &smci, nullptr, &shader_module_) != VK_SUCCESS) {
    SetLastError("vkCreateShaderModule failed (invalid SPIR-V?)");
    return false;
  }
  if (!pipeline_layout_) {
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
      SetLastError("vkCreatePipelineLayout failed");
      return false;
    }
  }
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader_module_;
  stage.pName = "main";
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage = stage;
  cpci.layout = pipeline_layout_;
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_) != VK_SUCCESS) {
    SetLastError("vkCreateComputePipelines failed");
    return false;
  }
  has_pipeline_ = true;
  std::fprintf(stderr, "[vulkan-backend] compute pipeline created from SPIR-V (%zu words)\n", word_count);
  return true;
}

void VulkanVideoBackend::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
  ++dispatches_;
  if (!ready_ || !has_pipeline_) {
    return;
  }
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = cmd_pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb;
  if (vkAllocateCommandBuffers(device_, &ai, &cb) != VK_SUCCESS) {
    SetLastError("vkAllocateCommandBuffers failed");
    return;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdDispatch(cb, x, y, z);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
    SetLastError("vkQueueSubmit failed");
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cb);
    return;
  }
  vkQueueWaitIdle(queue_);
  vkFreeCommandBuffers(device_, cmd_pool_, 1, &cb);
  std::fprintf(stderr, "[vulkan-backend] dispatched compute (%u,%u,%u) OK\n", x, y, z);
}

// 以下 IVideoBackend 方法当前为骨架占位（图形/上屏路径后续接入）。
void VulkanVideoBackend::SetBufferAttribute(uint32_t, const VideoBufferAttr&) {}
void VulkanVideoBackend::RegisterBuffer(uint32_t, uint64_t, uint32_t) {}
void VulkanVideoBackend::Flip(uint32_t, int64_t) { ++flips_; }
void VulkanVideoBackend::WaitVblank() {}
void VulkanVideoBackend::SubmitCommandBuffer(const uint32_t*, size_t) {}
void VulkanVideoBackend::DrawIndex(uint32_t, uint64_t, uint32_t) {}
void VulkanVideoBackend::SetRegister(uint32_t, uint32_t, bool) {}

}  // namespace divaft