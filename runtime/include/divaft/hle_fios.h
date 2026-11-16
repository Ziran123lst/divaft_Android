// divaft_Android — libSceFios2 HLE 注册（原创）
// Fios2 是 PS4 的文件 IO 中间件：FH(文件句柄)/DH(目录句柄)/Op(异步操作)/Archive(psarc)/IOFilter。
// 同步操作直接映射宿主 POSIX；异步操作内联完成 + 假 op 句柄 + 可选 guest 回调
// （模型参照 AstroQuest shadps4-arm64 的 fios2，GPL-2.0；签名经其实现核对）。
// 路径映射：/app0 与 ArchiveMount 挂载点 → 游戏根目录（DIVA_GAME_ROOT 或 CWD）。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "divaft/hle_wire.h"
#include "divaft/guest_bridge.h"

namespace divaft {

// 注册 libSceFios2 全部 28 个契约函数。bridge 供异步 op 完成时回调 guest 函数（可为 nullptr）。
size_t RegisterFiosHle(HleWire& wire, GuestBridge* bridge);

}  // namespace divaft
