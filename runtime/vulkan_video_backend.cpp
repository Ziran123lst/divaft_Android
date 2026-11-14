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

// ---- 图形管线离屏渲染（shaders→Vulkan 消费端，可见三角形验证）----
namespace {
uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  return UINT32_MAX;
}
}  // namespace

bool VulkanVideoBackend::CreateGraphicsPipelineFromSpirv(const uint32_t* vert, size_t vert_words,
                                                         const uint32_t* frag, size_t frag_words) {
  if (!ready_) {
    SetLastError("backend not initialized");
    return false;
  }
  VkShaderModuleCreateInfo vmci{};
  vmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  vmci.codeSize = vert_words * 4;
  vmci.pCode = vert;
  VkShaderModule vm = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &vmci, nullptr, &vm) != VK_SUCCESS) {
    SetLastError("vertex vkCreateShaderModule failed");
    return false;
  }
  VkShaderModuleCreateInfo fmci{};
  fmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fmci.codeSize = frag_words * 4;
  fmci.pCode = frag;
  VkShaderModule fm = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &fmci, nullptr, &fm) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vm, nullptr);
    SetLastError("fragment vkCreateShaderModule failed");
    return false;
  }
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout;
  if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vm, nullptr);
    vkDestroyShaderModule(device_, fm, nullptr);
    SetLastError("graphics vkCreatePipelineLayout failed");
    return false;
  }
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vm;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fm;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{0, 0, 256, 256, 0, 1};
  VkRect2D sc{{0, 0}, {256, 256}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &cba;
  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp;
  if (vkCreateRenderPass(device_, &rpci, nullptr, &rp) != VK_SUCCESS) {
    SetLastError("vkCreateRenderPass failed");
    vkDestroyPipelineLayout(device_, layout, nullptr);
    vkDestroyShaderModule(device_, vm, nullptr);
    vkDestroyShaderModule(device_, fm, nullptr);
    return false;
  }
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &blend;
  gpci.layout = layout;
  gpci.renderPass = rp;
  VkPipeline pipeline;
  VkResult res = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
  vkDestroyRenderPass(device_, rp, nullptr);
  vkDestroyPipelineLayout(device_, layout, nullptr);
  vkDestroyShaderModule(device_, vm, nullptr);
  vkDestroyShaderModule(device_, fm, nullptr);
  if (res != VK_SUCCESS) {
    SetLastError("vkCreateGraphicsPipelines failed (recompiler SPIR-V rejected?)");
    return false;
  }
  vkDestroyPipeline(device_, pipeline, nullptr);
  std::fprintf(stderr, "[vulkan-backend] graphics pipeline created from SPIR-V (vert %zu + frag %zu words)\n",
               vert_words, frag_words);
  return true;
}

bool VulkanVideoBackend::RenderGraphicsOffscreen(uint32_t w, uint32_t h, const uint32_t* vert,
                                                 size_t vert_words, const uint32_t* frag,
                                                 size_t frag_words, std::vector<uint8_t>& out_rgba) {
  if (!ready_) {
    SetLastError("backend not initialized");
    return false;
  }
  // shader modules
  VkShaderModuleCreateInfo vmci{};
  vmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  vmci.codeSize = vert_words * 4;
  vmci.pCode = vert;
  VkShaderModule vm;
  if (vkCreateShaderModule(device_, &vmci, nullptr, &vm) != VK_SUCCESS) {
    SetLastError("render: vertex module failed");
    return false;
  }
  VkShaderModuleCreateInfo fmci{};
  fmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fmci.codeSize = frag_words * 4;
  fmci.pCode = frag;
  VkShaderModule fm;
  if (vkCreateShaderModule(device_, &fmci, nullptr, &fm) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vm, nullptr);
    SetLastError("render: fragment module failed");
    return false;
  }
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout;
  vkCreatePipelineLayout(device_, &plci, nullptr, &layout);
  // render pass
  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  VkRenderPass rp;
  if (vkCreateRenderPass(device_, &rpci, nullptr, &rp) != VK_SUCCESS) {
    SetLastError("render: render pass failed");
    vkDestroyPipelineLayout(device_, layout, nullptr);
    vkDestroyShaderModule(device_, vm, nullptr);
    vkDestroyShaderModule(device_, fm, nullptr);
    return false;
  }
  // graphics pipeline
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vm;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fm;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  VkRect2D sc{{0, 0}, {w, h}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &cba;
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &blend;
  gpci.layout = layout;
  gpci.renderPass = rp;
  VkPipeline pipeline;
  if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS) {
    SetLastError("render: graphics pipeline failed");
    vkDestroyRenderPass(device_, rp, nullptr);
    vkDestroyPipelineLayout(device_, layout, nullptr);
    vkDestroyShaderModule(device_, vm, nullptr);
    vkDestroyShaderModule(device_, fm, nullptr);
    return false;
  }
  // offscreen image
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage image;
  if (vkCreateImage(device_, &ici, nullptr, &image) != VK_SUCCESS) {
    SetLastError("render: image failed");
    return false;
  }
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device_, image, &mr);
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = FindMemoryType(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory img_mem;
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &img_mem) != VK_SUCCESS) {
    SetLastError("render: image memory failed");
    vkDestroyImage(device_, image, nullptr);
    return false;
  }
  vkBindImageMemory(device_, image, img_mem, 0);
  VkImageViewCreateInfo ivci{};
  ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivci.image = image;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view;
  vkCreateImageView(device_, &ivci, nullptr, &view);
  VkFramebufferCreateInfo fbci{};
  fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbci.renderPass = rp;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &view;
  fbci.width = w;
  fbci.height = h;
  fbci.layers = 1;
  VkFramebuffer fb;
  vkCreateFramebuffer(device_, &fbci, nullptr, &fb);
  // readback buffer
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = w * h * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf;
  vkCreateBuffer(device_, &bci, nullptr, &buf);
  VkMemoryRequirements bmr;
  vkGetBufferMemoryRequirements(device_, buf, &bmr);
  VkMemoryAllocateInfo bmai{};
  bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  bmai.allocationSize = bmr.size;
  bmai.memoryTypeIndex = FindMemoryType(physical_, bmr.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory buf_mem;
  if (bmai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &bmai, nullptr, &buf_mem) != VK_SUCCESS) {
    SetLastError("render: readback memory failed");
    return false;
  }
  vkBindBufferMemory(device_, buf, buf_mem, 0);
  // record
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = cmd_pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(device_, &ai, &cb);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderPassBeginInfo rpbi{};
  rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpbi.renderPass = rp;
  rpbi.framebuffer = fb;
  rpbi.renderArea = {{0, 0}, {w, h}};
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;
  vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRenderPass(cb);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
    SetLastError("render: submit failed");
    return false;
  }
  vkQueueWaitIdle(queue_);
  void* mapped = nullptr;
  vkMapMemory(device_, buf_mem, 0, w * h * 4, 0, &mapped);
  out_rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + w * h * 4);
  vkUnmapMemory(device_, buf_mem);
  vkFreeCommandBuffers(device_, cmd_pool_, 1, &cb);
  vkDestroyBuffer(device_, buf, nullptr);
  vkFreeMemory(device_, buf_mem, nullptr);
  vkDestroyFramebuffer(device_, fb, nullptr);
  vkDestroyImageView(device_, view, nullptr);
  vkDestroyImage(device_, image, nullptr);
  vkFreeMemory(device_, img_mem, nullptr);
  vkDestroyPipeline(device_, pipeline, nullptr);
  vkDestroyRenderPass(device_, rp, nullptr);
  vkDestroyPipelineLayout(device_, layout, nullptr);
  vkDestroyShaderModule(device_, vm, nullptr);
  vkDestroyShaderModule(device_, fm, nullptr);
  std::fprintf(stderr, "[vulkan-backend] offscreen graphics render %ux%u done (%zu bytes readback)\n",
               w, h, out_rgba.size());
  return true;
}

}  // namespace divaft