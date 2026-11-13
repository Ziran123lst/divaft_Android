// divaft_Android — libkernel HLE 注册（原创）
// 内存管理 + 时间/CPU + 同步原语（equeue / pthread 互斥锁 / 条件变量 / 线程属性）+ 线程创建。
// 同步对象用宿主 backing，按 guest 指针地址索引（不假设 guest 结构体布局）。
// pthread_create 用宿主线程调 GuestBridge::CallGuest 执行 guest 线程例程
// （桥接已支持 per-call 栈 + 返回蹦床，见 guest_bridge.cpp 里程碑23）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"
#include "divaft/guest_bridge.h"

namespace divaft {

// 注册 libkernel 内存/时间/同步/线程 HLE 到 wire。bridge 供 pthread_create 执行 guest 例程。
// 返回成功注册条数。
size_t RegisterKernelHle(HleWire& wire, GuestMemoryManager* mem, GuestBridge* bridge);

}  // namespace divaft