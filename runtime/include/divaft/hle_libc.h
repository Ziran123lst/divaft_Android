// divaft_Android — libc HLE 注册（原创）
// 把 DIVA 自带 libc 的关键导入映射到宿主实现。
// 分配类（malloc/free/...）走 GuestHeap（返回 guest 地址）；
// 内存/字符串/数学/stdio 直接映射宿主（guest 指针恒等映射可直接解引用）。
// C++ ABI / MSVC 线程(_Mtx/_Cnd/_Thrd) / locale 等暂留记录型 stub，待真机日志定优先级。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/guest_memory.h"

namespace divaft {

// 注册 libc 关键 HLE。heap 为 guest 堆（分配类使用）。返回成功注册条数。
size_t RegisterLibcHle(HleWire& wire, GuestHeap* heap);

}  // namespace divaft