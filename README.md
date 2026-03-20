# 東方紅魔郷 SDL2 移植版 / th06-sdl2

**[English](#english)** | **[中文](#中文)**

---

<a id="english"></a>

## English

### About

This is an SDL2/OpenGL port of [Touhou Koumakyou ~ the Embodiment of Scarlet Devil (東方紅魔郷)](https://en.touhouwiki.net/wiki/Embodiment_of_Scarlet_Devil) v1.02h, based on the [re-engineered source code](https://github.com/happyhavoc/th06) by the community.

The original project reconstructs the game source code from the binary to achieve a byte-accurate match with the original executable. This fork replaces the Direct3D 8 / Win32 backend with **SDL2 + OpenGL**, aiming for cross-platform compatibility.

### Features

- **SDL2 + OpenGL / OpenGL ES 2.0** rendering backend (replacing Direct3D 8)
- **OpenGL ES (GLES) renderer** — dedicated `RendererGLES` backend with FBO-based fullscreen scaling
- **SDL2_mixer** for audio playback (replacing DirectSound)
- **SDL2_image** for texture loading
- **Runtime encoding detection** — automatically detects GBK (Chinese) and Shift-JIS (Japanese) game data, no recompilation needed
- **thprac integration** — built-in practice mode overlay (stage select, practice tools)
- **CMake** build system
- **Cross-platform**: Windows (x86/x64), Linux (x86/x64, GLES), Android (WIP)

### Requirements

- CMake >= 3.20
- MSVC (Visual Studio 2019+), GCC 13+, or compatible C++17 compiler
- Python 3 (for `i18n.hpp` generation)
- Original game data files (`東方紅魔郷.exe` v1.02h)

### Building

```bash
# Windows (Win32, with GLES renderer)
cmake -B build_sdl2 -A Win32 -DUSE_GLES=ON
cmake --build build_sdl2 --config Release

# Linux (32-bit GLES)
PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig \
cmake -B build_linux_gles -DCMAKE_BUILD_TYPE=Release -DUSE_GLES=ON
cmake --build build_linux_gles
```

SDL2 libraries are bundled in `3rdparty/` for Windows and linked automatically. On Linux, install SDL2/SDL2_image/SDL2_mixer via your package manager.

### Running

The executable needs access to the original game data. Place the game data files (or `build/` directory from the original game) alongside the executable, or run from the game data directory:

```bash
cd <path-to-game-data>
<path-to>/build_sdl2/Release/th06.exe
```

### Project Structure

```
CMakeLists.txt              # SDL2 build system (USE_GLES option)
3rdparty/
  SDL2/                     # SDL2 development libraries
  SDL2_image/               # SDL2_image development libraries
  SDL2_mixer/               # SDL2_mixer development libraries
  imgui/                    # Dear ImGui (thprac overlay)
  Detours/                  # Microsoft Detours (hook library)
  rapidjson/                # JSON parser
src/
  sdl2_renderer.cpp/.hpp    # Desktop OpenGL rendering backend
  RendererGLES.cpp/.hpp     # OpenGL ES 2.0 rendering backend
  IRenderer.hpp             # Abstract renderer interface
  TextHelper.cpp            # Text rendering (stb_truetype, runtime encoding)
  GameWindow.cpp            # SDL2 window management & frame timing
  thprac_*.cpp              # thprac practice mode integration
  *.cpp / *.hpp             # Game source (ported from D3D8)
scripts/                    # Build & i18n generation scripts
config/                     # Decompilation mapping data
android/                    # Android build (Gradle + NDK, WIP)
```

### Credits

- [happyhavoc/th06](https://github.com/happyhavoc/th06) — Original reverse-engineered source code
- [Team Shanghai Alice](https://www16.big.or.jp/~zun/) — Original game by ZUN
- [SDL2](https://www.libsdl.org/) / [OpenGL](https://www.opengl.org/)


### ⚠️ Developer Disclaimer (A Note on this Project's "Silicon Content")

> **TL;DR:** Yes, this project is 100% **vibe-coded**!
> 
> Confession time: A significant portion of the underlying code in this repo was generated with the help of LLMs. If you're scrolling through the commit history and catch a strong whiff of "AI," trust your instincts—your radar is spot on.
> 
> My primary goal was straightforward: **achieve cross-platform compatibility** and get the game running smoothly with **full OpenGL hardware acceleration on modern machines**, finally breaking free from the ancient shackles of D3D8. To quickly validate this idea and hit that goal, I happily outsourced all the tedious C++ grunt work, API swapping, and low-level duct-taping to my AI assistant.
> 
> The core philosophy here is simple: "The code might look a bit abstract, but hey, it actually runs on modern hardware." 🛠️

---

<a id="中文"></a>

## 中文

### 项目简介

本仓库是 [東方紅魔郷 ～ the Embodiment of Scarlet Devil](https://en.touhouwiki.net/wiki/Embodiment_of_Scarlet_Devil) v1.02h 的 **SDL2/OpenGL 移植版**，基于社区 [逆向重建的源代码](https://github.com/happyhavoc/th06)。

原项目通过逆向工程从二进制文件重建游戏源代码，目标是与原版可执行文件逐字节一致。本仓库将 Direct3D 8 / Win32 后端替换为 **SDL2 + OpenGL**，以实现跨平台兼容。

### 特性

- **SDL2 + OpenGL / OpenGL ES 2.0** 渲染后端（替代 Direct3D 8）
- **OpenGL ES (GLES) 渲染器** — 独立的 `RendererGLES` 后端，支持 FBO 全屏缩放
- **SDL2_mixer** 音频播放（替代 DirectSound）
- **SDL2_image** 纹理加载
- **运行时编码自动检测** — 自动识别 GBK（中文版）和 Shift-JIS（日文版）游戏数据，无需重新编译
- **thprac 集成** — 内置练习模式覆盖层（关卡选择、练习工具）
- **CMake** 构建系统
- **跨平台**：Windows (x86/x64)、Linux (x86/x64, GLES)、Android（开发中）

### 环境要求

- CMake >= 3.20
- MSVC（Visual Studio 2019+）、GCC 13+ 或兼容的 C++17 编译器
- Python 3（用于生成 `i18n.hpp`）
- 原版游戏数据文件（`東方紅魔郷.exe` v1.02h）

### 构建

```bash
# Windows（Win32，启用 GLES 渲染器）
cmake -B build_sdl2 -A Win32 -DUSE_GLES=ON
cmake --build build_sdl2 --config Release

# Linux（32 位 GLES）
PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig \
cmake -B build_linux_gles -DCMAKE_BUILD_TYPE=Release -DUSE_GLES=ON
cmake --build build_linux_gles
```

Windows 下 SDL2 库已内置于 `3rdparty/` 目录，自动链接。Linux 下请通过包管理器安装 SDL2/SDL2_image/SDL2_mixer。

### 运行

可执行文件需要访问原版游戏数据。将游戏数据文件（或原版游戏的 `build/` 目录）放在可执行文件旁，或从游戏数据目录运行：

```bash
cd <游戏数据路径>
<路径>/build_sdl2/Release/th06.exe
```

### 项目结构

```
CMakeLists.txt              # SDL2 构建系统（USE_GLES 选项）
3rdparty/
  SDL2/                     # SDL2 开发库
  SDL2_image/               # SDL2_image 开发库
  SDL2_mixer/               # SDL2_mixer 开发库
  imgui/                    # Dear ImGui（thprac 覆盖层）
  Detours/                  # Microsoft Detours（Hook 库）
  rapidjson/                # JSON 解析器
src/
  sdl2_renderer.cpp/.hpp    # 桌面 OpenGL 渲染后端
  RendererGLES.cpp/.hpp     # OpenGL ES 2.0 渲染后端
  IRenderer.hpp             # 抽象渲染器接口
  TextHelper.cpp            # 文字渲染（stb_truetype，运行时编码检测）
  GameWindow.cpp            # SDL2 窗口管理与帧计时
  thprac_*.cpp              # thprac 练习模式集成
  *.cpp / *.hpp             # 游戏源码（从 D3D8 移植）
scripts/                    # 构建与 i18n 生成脚本
config/                     # 反编译映射数据
android/                    # Android 构建（Gradle + NDK，开发中）
```

### 致谢

- [happyhavoc/th06](https://github.com/happyhavoc/th06) — 原始逆向重建源码
- [上海爱丽丝幻乐团](https://www16.big.or.jp/~zun/) — ZUN 制作的原版游戏
- [SDL2](https://www.libsdl.org/) / [OpenGL](https://www.opengl.org/)


### ⚠️ 开发者叠甲时间 (关于本项目含硅量的声明)

> **太长不看版：** 没错，这是一个纯正的 **Vibe-coded（凭感觉编程）** 项目！
> 
> 坦白局：本项目含有大量由 LLM辅助生成的底层代码。如果你在翻看 commit 记录时闻到了一股浓浓的“AI 味”，自信点，你的直觉非常准确。
> 
> 我的核心目的其实非常纯粹：**实现跨平台运行**，并让游戏在**现代机器上能顺利跑满 OpenGL 硬件加速**，彻底摆脱古老的 D3D8 的束缚。为了快速达成这个目标验证想法，那些繁琐的 C++ 搬砖活儿、API 替换和底层缝合，我就全权委托给 AI 助理了。
> 
> 主打一个“代码虽然抽象，但它在现代机器上真能跑”。🛠️
