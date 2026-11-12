// divaft_Android — 输入（触屏 → OrbisPadData / slide 合成）
// 原创接口。DIVA 的 slide 音符原生读触摸板轨迹（OrbisPadData.touchData），
// 本模块把屏幕滑动合成成 PS4 触摸板坐标（1920x942），游戏 slide 检测零修改。
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace divaft {

// PS4 按键位（OrbisPadButtonDataOffset）
constexpr uint32_t kPadCross = 0x4000;
constexpr uint32_t kPadCircle = 0x2000;
constexpr uint32_t kPadSquare = 0x8000;
constexpr uint32_t kPadTriangle = 0x1000;
constexpr uint32_t kPadL1 = 0x0400;
constexpr uint32_t kPadR1 = 0x0800;

constexpr int kTouchpadW = 1920;
constexpr int kTouchpadH = 942;

struct PadState {
  uint32_t buttons = 0;
  uint8_t touch_num = 0;
  uint16_t touch_x[2] = {0, 0};
  uint16_t touch_y[2] = {0, 0};
};

class InputPad {
public:
  // 演奏模式：屏幕分区 tap → 按键；滑动 → 触摸板轨迹。
  void OnTouchDown(int pointer_id, float x, float y, float screen_w, float screen_h);
  void OnTouchMove(int pointer_id, float x, float y, float screen_w, float screen_h);
  void OnTouchUp(int pointer_id);
  // 菜单模式：虚拟十字键/确认/返回。
  void SetMenuMode(bool menu);
  // 供 scePadRead 读取的最新状态（无锁快照）。
  PadState Snapshot() const;

private:
  PadState state_;
  bool menu_mode_ = false;
  uint32_t ZoneToButton(float x, float y, float w, float h) const;
};

}  // namespace divaft
