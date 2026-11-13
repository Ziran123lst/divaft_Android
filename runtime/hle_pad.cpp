// divaft_Android — 输入 HLE 实现（阶段5 骨架）
// OrbisPadData 关键偏移（对照 shadPS4 pad.h）：buttons@0, touchData@52, connected@76。
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/hle_pad.h"

#include <cstring>

namespace divaft {

namespace {
// 从 InputPad 快照填一份 OrbisPadData 到 guest 缓冲。
void FillPadData(uint64_t guest_buf, const PadState& s) {
  if (!guest_buf) return;
  uint8_t* d = reinterpret_cast<uint8_t*>(guest_buf);
  std::memset(d, 0, 128);
  *reinterpret_cast<uint32_t*>(d + 0) = s.buttons;         // buttons
  d[52] = s.touch_num;                                      // touchData.touchNum
  for (int i = 0; i < 2 && i < s.touch_num; ++i) {
    uint8_t* t = d + 60 + i * 8;                            // touch[i]
    *reinterpret_cast<uint16_t*>(t + 0) = s.touch_x[i];
    *reinterpret_cast<uint16_t*>(t + 2) = s.touch_y[i];
    t[4] = static_cast<uint8_t>(i);                         // id
  }
  d[76] = 1;                                                // connected
}
}  // namespace

size_t RegisterPadHle(HleWire& wire, InputPad* pad) {
  size_t n = 0;
  auto regp = [&](const char* name, HleFn fn) {
    if (wire.RegisterByName("libScePad", name, std::move(fn))) ++n;
  };
  regp("scePadInit", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadOpen", [](const uint64_t*, unsigned) -> uint64_t { return 1; });  // 句柄
  regp("scePadClose", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  // scePadRead(handle, data, num)
  regp("scePadRead", [pad](const uint64_t* a, unsigned) -> uint64_t {
    if (pad && a[1]) FillPadData(a[1], pad->Snapshot());
    return 1;  // 读到 1 份数据
  });
  // scePadReadState(handle, data)
  regp("scePadReadState", [pad](const uint64_t* a, unsigned) -> uint64_t {
    if (pad && a[1]) FillPadData(a[1], pad->Snapshot());
    return 0;
  });
  regp("scePadGetControllerInformation", [](const uint64_t* a, unsigned) -> uint64_t {
    if (a[1]) {
      uint8_t* d = reinterpret_cast<uint8_t*>(a[1]);
      std::memset(d, 0, 64);
      // touchPadInfo.resolution = 1920x942
      *reinterpret_cast<uint16_t*>(d + 8) = 1920;
      *reinterpret_cast<uint16_t*>(d + 10) = 942;
      d[16] = 1;  // connected
    }
    return 0;
  });
  regp("scePadSetLightBar", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadResetLightBar", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadSetVibration", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadResetOrientation", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadSetAngularVelocityDeadbandState", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  regp("scePadSetTiltCorrectionState", [](const uint64_t*, unsigned) -> uint64_t { return 0; });
  return n;
}

}  // namespace divaft