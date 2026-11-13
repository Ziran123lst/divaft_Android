// divaft_Android — 视频后端接口（图形骨架，阶段3）
// 原创抽象：把 GnmDriver/VideoOut HLE 与具体渲染后端（Vulkan/Turnip）解耦。
// 当前提供 NullBackend 占位；后续接 shadPS4 video_core/renderer_vulkan + Turnip(KGSL)。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace divaft {

// 显示缓冲属性（对应 sceVideoOutSetBufferAttribute 的关键字段）。
struct VideoBufferAttr {
  uint32_t pixel_format = 0;   // OrbisVideoOutPixelFormat
  uint32_t tiling_mode = 0;
  uint32_t aspect_ratio = 0;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t pitch_in_pixel = 1920;
};

// 渲染后端抽象。GnmDriver 命令缓冲 / VideoOut 翻页都经此下发。
class IVideoBackend {
public:
  virtual ~IVideoBackend() = default;
  virtual bool Init(uint32_t width, uint32_t height) = 0;
  virtual void SetBufferAttribute(uint32_t index, const VideoBufferAttr& attr) = 0;
  virtual void RegisterBuffer(uint32_t index, uint64_t gpu_addr, uint32_t pitch) = 0;
  virtual void Flip(uint32_t index, int64_t flip_arg) = 0;
  virtual void WaitVblank() = 0;
  // 提交 GnmDriver 命令缓冲（PM4 包）。后端翻译为 Vulkan 命令。
  virtual void SubmitCommandBuffer(const uint32_t* cmdbuf, size_t dwords) = 0;
  virtual const char* name() const = 0;
};

// 占位后端：记录调用、无渲染。Vulkan/Turnip 接入前的骨架。
class NullVideoBackend final : public IVideoBackend {
public:
  bool Init(uint32_t w, uint32_t h) override { w_ = w; h_ = h; return true; }
  void SetBufferAttribute(uint32_t, const VideoBufferAttr&) override {}
  void RegisterBuffer(uint32_t, uint64_t, uint32_t) override {}
  void Flip(uint32_t, int64_t) override { ++flips_; }
  void WaitVblank() override {}
  void SubmitCommandBuffer(const uint32_t*, size_t) override { ++cmdbufs_; }
  const char* name() const override { return "null"; }
  uint64_t flips() const { return flips_; }
  uint64_t cmdbufs() const { return cmdbufs_; }
private:
  uint32_t w_ = 0, h_ = 0;
  uint64_t flips_ = 0, cmdbufs_ = 0;
};

}  // namespace divaft