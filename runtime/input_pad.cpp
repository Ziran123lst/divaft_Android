// divaft_Android — 输入实现（原创）
// SPDX-License-Identifier: GPL-2.0-or-later
#include "divaft/input_pad.h"

#include <cmath>

namespace divaft {

// 演奏模式屏幕四分区 → 四键（左下=□ 左上=△ 右下=× 右上=○，可配置）。
uint32_t InputPad::ZoneToButton(float x, float y, float w, float h) const {
  bool left = x < w * 0.5f;
  bool top = y < h * 0.5f;
  if (left && top) return kPadTriangle;
  if (left && !top) return kPadSquare;
  if (!left && top) return kPadCircle;
  return kPadCross;
}

void InputPad::OnTouchDown(int pointer_id, float x, float y, float w, float h) {
  if (menu_mode_) return;  // 菜单模式由虚拟按键处理
  if (pointer_id > 1) return;
  state_.buttons |= ZoneToButton(x, y, w, h);
  // 起始触点映射到触摸板（供 slide 起始）
  state_.touch_num = 1;
  state_.touch_x[0] = static_cast<uint16_t>(x / w * kTouchpadW);
  state_.touch_y[0] = static_cast<uint16_t>(y / h * kTouchpadH);
}

void InputPad::OnTouchMove(int pointer_id, float x, float y, float w, float h) {
  if (menu_mode_ || pointer_id > 1) return;
  // 滑动 → 触摸板轨迹（游戏 slide 检测原生读取）
  state_.touch_num = 1;
  state_.touch_x[0] = static_cast<uint16_t>(x / w * kTouchpadW);
  state_.touch_y[0] = static_cast<uint16_t>(y / h * kTouchpadH);
}

void InputPad::OnTouchUp(int pointer_id) {
  if (pointer_id > 1) return;
  state_.buttons = 0;
  state_.touch_num = 0;
}

void InputPad::SetMenuMode(bool menu) { menu_mode_ = menu; }

PadState InputPad::Snapshot() const { return state_; }

}  // namespace divaft
