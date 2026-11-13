// divaft_Android — libkernel HLE 注册（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"

namespace divaft {

// 注册 libkernel 内存管理 + 时间/CPU HLE 到 wire。
void RegisterKernelHle(HleWire& wire, GuestMemoryManager* mem);

}  // namespace divaft
