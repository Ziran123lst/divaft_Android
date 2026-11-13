// divaft_Android — 图形 HLE（libSceGnmDriver + libSceVideoOut，阶段3 骨架）
// GnmDriver：PM4 命令缓冲/着色器/绘制 API，当前记录型 stub，后续接 video_core 翻译。
// VideoOut：显示输出，接 IVideoBackend（当前 NullBackend，后续 Vulkan/Turnip）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/video_backend.h"

namespace divaft {

// 注册 GnmDriver(103) + VideoOut(8)。backend 为渲染后端（可为 NullVideoBackend）。
// 返回成功注册条数。
size_t RegisterGraphicsHle(HleWire& wire, IVideoBackend* backend);

}  // namespace divaft