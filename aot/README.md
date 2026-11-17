# DIVA Future Tone DX — AOT 静态重编译路线（性能最优解）

《初音未来 歌姬计划 Future Tone DX》(PS4, CUSA08053) → Android/ARM64 原生移植。
本目录是 2026-11-16 起的 **AOT 路线**全部工程：补丁、构建脚本、验证脚本、文档。
原 FEX-JIT 路线（`runtime/`）保留作参考；AOT 路线性能为理论天花板（零运行时 JIT）。

## 为什么换 AOT 路线

原路线（FEX JIT + 自写 HLE）卡在 C++ locale/iostream ABI 墙（约 39 个繁重符号）。
调研命中现成工具链：

- **bb-recomp**（ducvd89/Bloodborne-android-rebuild-tool，GPL-3.0-or-later）：
  FEXCore 前端 → FEX IR → LLVM IR → AArch64 `.so` 的静态重编译器，通用非血源专属。
  关键：把 `libc.prx + libSceFios2.prx` **离线链接进单一镜像**一起翻译——C++ 运行时
  以原生 guest 代码跑，locale 墙直接消失。
- **Bloodborne-Android**（ducvd89/Bloodborne-Android，GPL-2.0-or-later）：
  配套完整运行时 = AOT CPU 引擎（cpu/）+ shadPS4 图形核心（gpu/shadps4）+
  bbport 式 HLE（src/runtime*.c）+ Android launcher。架构高度通用
  （"游戏无 profile 也能走通用路径"）。

## 已达成（2026-11-17）

1. **DIVA 100% 原生 AOT**：57133 单元 / 27490 函数，0 不可翻译。
   为此给 bb-recomp 编译器补了 3 条缺失指令的 lowering：
   - `VLoadVectorMasked` / `VStoreVectorMasked`（AVX vmaskmov，LLVM masked.load/store + 符号位掩码）
   - `VPCMPISTRX`（SSE4.2 字符串比较，C helper `bbr_pcmpistrx.c` 移植 FEX MIT 参考实现）
2. **完整运行时双版本构建**：bionic（手机）+ glibc ARM64（qemu 无手机验证）。
3. **qemu 全 GPU 模式自主跑通**：Xvfb + SDL x11 + lavapipe 软件 Vulkan，
   327 个 HLE 符号注册（GnmDriver/VideoOut/AvPlayer），游戏写真实 PM4 命令缓冲。
4. **DIVA 导入 100% 解析**：NID 字典 709 条 + scope 别名 404 条
   （DIVA 与血源 scope 后缀不同、raw NID 通用）+ 自写 HLE 模块
   （Ngs2 音频 11 函数、DiscMap/SystemService/NP/对话框等）。
5. **游戏原生执行到渲染阶段**：libc/Fios2 模块初始化 → AppContent → 音频初始化 →
   渲染线程创建 → GnmDriver 绘图命令提交。

当前停点：eboot unit+0x555370（std::string 清理路径）空指针解引用 address 0x4，
发生在 sceGnmInsertPushMarker/PopMarker 调用后。下一步：给 AOT 故障处理器加
guest 寄存器 dump 定位空值来源。

## 目录

```
aot/
  patches/
    apply_diva_patches.sh      # 幂等补丁脚本（bb-recomp + bb-android 全部改动）
    bbr_pcmpistrx.c            # SSE4.2 字符串比较 helper（MIT，移植自 FEX）
    runtime_ngs2.c.diva        # 最小 Ngs2 音频 HLE（11 函数，有效句柄+静音渲染）
    runtime_diva_misc.c.diva   # DiscMap/SystemService/NP/对话框等 HLE
    runtime_kernel.c.diva      # kernel 增补（IsNeoMode/ASAN/时间转换安全实现）
  scripts/
    diva_bionic_deps.sh        # bionic 依赖（跳过在线模块；ffmpeg/SDL3/zydis/...）
    diva_glibc_deps.sh         # glibc 精简依赖（sysroot 已有大部分；Vulkan头/Boost/miniz）
    diva_bionic_build.sh       # bionic 运行时构建（BB_NET=OFF）
    diva_arm64_build.sh        # glibc ARM64 运行时构建（qemu 可跑）
    fetch_sysroot.sh           # Debian trixie arm64 sysroot + lavapipe（aliyun 镜像）
    run_qemu_gpu.sh            # 全 GPU 模式 qemu 验证（Xvfb+lavapipe 一键）
    restore_aot.sh             # 环境重置后一键恢复（工具二进制+NDK+AOT 产物）
```

## 完整流程（环境重置后）

```bash
# 0. 系统工具
apt-get install -y zstd cmake ninja-build zlib1g-dev libzstd-dev libxml2-dev \
    pkg-config glslang-tools qemu-user unzip xvfb git python3
# 1. 克隆上游（浅克隆即可）
git clone --depth 1 https://github.com/ducvd89/Bloodborne-android-rebuild-tool /root/divaft-work/bb-recomp
git clone --depth 1 https://github.com/ducvd89/Bloodborne-Android /root/divaft-work/bb-android
# 2. 恢复持久化产物（工具二进制/NDK/AOT 库/boot-linked 镜像；见云盘 divaft-persist）
bash aot/scripts/restore_aot.sh
# 3. 应用全部 DIVA 补丁（幂等）
bash aot/patches/apply_diva_patches.sh
# 4. glibc 验证链
bash aot/scripts/fetch_sysroot.sh          # sysroot + lavapipe
bash aot/scripts/diva_glibc_deps.sh        # 精简依赖
cd /root/divaft-work/bb-android && git submodule update --init \
    gpu/third_party/fsr-vulkan gpu/third_party/imgui third_party/LibAtrac9
bash aot/scripts/diva_arm64_build.sh       # bb-probe + libbbcpu + libbbgpu
bash aot/scripts/run_qemu_gpu.sh <游戏目录CUSA08053>
# 5. 手机链（bionic）
bash aot/scripts/diva_bionic_deps.sh && bash aot/scripts/diva_bionic_build.sh
```

AOT 库重编（如需）：`BB_SKIP_GAME_CHECK=1 bb-recomp --game <CUSA08053目录>
--out <输出> --no-variants --keep-work --jobs 4`。
glibc 目标用 fake-ndk 包装（clang 转 `--target=aarch64-linux-gnu --sysroot=...
-fuse-ld=lld`，builtins 用 NDK 的 compiler-rt）；bionic 目标用真 NDK。
产物 `libbbaot.so` + `libbbaot.json` + `.bbwork/data/boot-linked.bin` + `content.bin`。

## 关键技术点

- **BB_SKIP_GAME_CHECK=1**：跳过工具的血源校验（无需 patch 二进制）。
- **NID 通用性**：PS4 NID 跨游戏通用，scope 后缀（#q#n vs #q#q）因游戏而异。
  别名表在 runtime_resolve 入口把 DIVA scope 转成血源 scope，现有解析器零改动。
- **BB_IMPORT_STUBS=1**：未解析导入记录并返回 0（探索模式），不再直接停进程。
- **qemu 跑 bionic 不行**（缺 /system/bin/linker64），故做 glibc ARM64 版；
  Debian 的 libm.so 等是 linker script，需 syslibs 符号链接指向 .so.6。
- **lavapipe**：Debian mesa-vulkan-drivers arm64 包；VK_ICD_FILENAMES 指绝对路径 json。
- **环境频繁重置**：一切产物/补丁/脚本持久化到云盘；/root 只当临时构建盘。

## 许可与资产

- 本目录补丁/脚本为原创，GPL-2.0-or-later（与主仓库一致）；
  bbr_pcmpistrx.c 移植自 FEX（MIT），文件头已标注。
- 上游 bb-recomp GPL-3.0-or-later、Bloodborne-Android GPL-2.0-or-later，
  以补丁形式对接，不复制其源码进本仓库。
- **游戏资产与派生物不入库**：eboot/镜像/libbbaot.so/boot-linked.bin 均源自
  用户自有游戏，工具明示不得分发；仅存用户私有云盘。
