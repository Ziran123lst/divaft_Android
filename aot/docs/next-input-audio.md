# 下一步：触屏输入 + AAudio 出声（输入/音频路径分析）

## 输入
- Android(BB_NO_SDL) 实时输入链路: runtime_pad.c sample_host() -> bbgpu_mouse_state() -> g_window->GetMouseState() -> apply_mouse() 映射到 OrbisPadData。
- 现状: Android 无 SDL window 时 g_window 为空 -> BbMouseState{} focused=0 -> 中性输入。需让 shim 接受 launcher 触屏状态。
- 方案: 在 gpu/shim 增加 Android 触屏输入源（共享内存/文件/JNI setter），launcher DisplayActivity 触屏覆层写入；或导出 bbgpu_set_mouse(x,y,buttons,focused) 供 app 调用。需重编 libbbgpu。
- 按键位: BTN_CROSS=0x4000 BTN_CIRCLE=0x2000 BTN_SQUARE=0x8000 BTN_TRIANGLE=0x1000 BTN_OPTIONS=0x8 BTN_TOUCHPAD=0x100000。PadData=120B, touches@60, timestamp@80。
- BB_PAD_FILE 仅脚本化自动测试（ms buttons 行/replay），不用于实时。

## 音频
- 主音频引擎=FMOD(静态链入 eboot, 原生 AOT) -> sceAudioOutOutput -> runtime_audio.c。
- Android(BB_NO_SDL) 当前 audio backend=timer(silent)。需接 AAudio: 在 runtime_audio.c 增加 AAudio 后端（AAudioStreamBuilder, 48kHz s16 stereo, 回调从 ring 读），或 launcher 读 FIFO(BB_AUDIO_FIFO) 经 AAudio 播放。
- Ngs2 现为最小 HLE(静音)；完整 DSP 可选移植 shadPS4 ngs2。

## 验证
- qemu 已可复现 boot（含 cond 死锁，BB_COND_BREAKER 修复）。缺 ram/farc 数据停点 0x48c2e0 为数据依赖，真机全数据可越过。
