// divaft_Android — 图形 HLE 实现（阶段3 骨架）
// GnmDriver 整库记录型 stub（PM4 命令缓冲翻译待接 video_core）；VideoOut 接 IVideoBackend。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_graphics.h"
#include "divaft/pm4_parser.h"

#include <cstdio>
#include <cstring>
#include <atomic>

namespace divaft {

namespace {
std::atomic<uint64_t> g_gnm_calls{0};
}

size_t RegisterGraphicsHle(HleWire& wire, IVideoBackend* backend) {
  size_t n = 0;

  // ---- libSceGnmDriver：整库记录型 stub（103 函数）----
  // 命令缓冲/着色器/绘制 API 的翻译是阶段3 主体工作，此处先记录调用以便观察启动序列。
  n += wire.RegisterLibrary("libSceGnmDriver",
      [](const uint64_t*, unsigned) -> uint64_t {
        g_gnm_calls.fetch_add(1);
        return 0;  // ORBIS_OK
      });

  // 命令提交：把 DCB（draw command buffer）喂给 PM4 解析器 → 渲染后端。
  // sceGnmSubmitCommandBuffers(count, dcb_gpu_addrs, dcb_sizes, ccb_gpu_addrs, ccb_sizes)
  auto submit = [backend](const uint64_t* a, unsigned) -> uint64_t {
    uint32_t count = (uint32_t)a[0];
    uint64_t dcb_addrs = a[1], dcb_sizes = a[2];
    if (backend && dcb_addrs && dcb_sizes && count <= 16) {
      const uint64_t* addrs = reinterpret_cast<const uint64_t*>(dcb_addrs);
      const uint32_t* sizes = reinterpret_cast<const uint32_t*>(dcb_sizes);
      for (uint32_t i = 0; i < count; ++i) {
        if (!addrs[i] || sizes[i] < 4) continue;
        const uint32_t* cmdbuf = reinterpret_cast<const uint32_t*>(addrs[i]);
        backend->SubmitCommandBuffer(cmdbuf, sizes[i] / 4);
        PM4Parser::Parse(cmdbuf, sizes[i] / 4, backend);
      }
    }
    return 0;
  };
  auto regg = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libSceGnmDriver", name, std::move(fn))) {}
  };
  regg("sceGnmSubmitCommandBuffers", submit);
  regg("sceGnmSubmitCommandBuffersForWorkload", submit);
  regg("sceGnmSubmitAndFlipCommandBuffers", submit);
  regg("sceGnmSubmitAndFlipCommandBuffersForWorkload", submit);
  regg("sceGnmSubmitDone", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  // ---- libSceVideoOut：显示输出，接渲染后端 ----
  auto regv = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libSceVideoOut", name, std::move(fn))) ++n;
  };
  // sceVideoOutOpen(userId, type, index, param) -> handle
  regv("sceVideoOutOpen", [backend](const uint64_t*, unsigned) -> uint64_t {
    if (backend) backend->Init(1920, 1080);
    return 1;  // 视频输出句柄
  });
  // sceVideoOutSetBufferAttribute(attr, pixelFormat, tiling, aspect, w, h, pitch)
  regv("sceVideoOutSetBufferAttribute", [backend](const uint64_t* a, unsigned) -> uint64_t {
    VideoBufferAttr attr;
    attr.pixel_format = (uint32_t)a[1];
    attr.tiling_mode = (uint32_t)a[2];
    attr.aspect_ratio = (uint32_t)a[3];
    attr.width = (uint32_t)a[4];
    attr.height = (uint32_t)a[5];
    attr.pitch_in_pixel = (uint32_t)a[6];
    if (backend) backend->SetBufferAttribute(0, attr);
    if (a[0]) std::memcpy(reinterpret_cast<void*>(a[0]), &attr, sizeof(attr) < 32 ? sizeof(attr) : 32);
    return 0;
  });
  // sceVideoOutRegisterBuffers(handle, startIndex, addresses, num, attr)
  regv("sceVideoOutRegisterBuffers", [backend](const uint64_t* a, unsigned) -> uint64_t {
    uint32_t start = (uint32_t)a[1];
    uint64_t addrs = a[2];
    uint32_t num = (uint32_t)a[3];
    if (backend && addrs) {
      for (uint32_t i = 0; i < num && i < 16; ++i) {
        uint64_t gpu_addr = reinterpret_cast<const uint64_t*>(addrs)[i];
        backend->RegisterBuffer(start + i, gpu_addr, 1920);
      }
    }
    return 0;
  });
  regv("sceVideoOutSetFlipRate", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  // sceVideoOutGetFlipStatus(handle, status) -> 填状态
  regv("sceVideoOutGetFlipStatus", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[1]) std::memset(reinterpret_cast<void*>(a[1]), 0, 64);
    return 0;
  });
  regv("sceVideoOutWaitVblank", [backend](const uint64_t*, unsigned) -> uint64_t {
    if (backend) backend->WaitVblank();
    return 0;
  });
  regv("sceVideoOutAdjustColor", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regv("sceVideoOutColorSettingsSetGamma", [](const uint64_t*, unsigned) -> uint64_t { return 0; });

  return n;
}

}  // namespace divaft