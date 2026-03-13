# 東方紅魔郷 SDL2 移植版 / th06-sdl2

**[English](#english)** | **[中文](#中文)**

---

<a id="english"></a>

## English

### About

This is an SDL2/OpenGL port of [Touhou Koumakyou ~ the Embodiment of Scarlet Devil (東方紅魔郷)](https://en.touhouwiki.net/wiki/Embodiment_of_Scarlet_Devil) v1.02h, based on the [re-engineered source code](https://github.com/happyhavoc/th06) by the community.

The original project reconstructs the game source code from the binary to achieve a byte-accurate match with the original executable. This fork replaces the Direct3D 8 / Win32 backend with **SDL2 + OpenGL**, aiming for cross-platform compatibility.

### Features

- **SDL2 + OpenGL** rendering backend (replacing Direct3D 8)
- **SDL2_mixer** for audio playback (replacing DirectSound)
- **SDL2_image** for texture loading
- **CMake** build system
- Windows x86/x64 support (other platforms planned)

### Requirements

- CMake >= 3.20
- MSVC (Visual Studio 2019+) or compatible C++17 compiler
- Python 3 (for `i18n.hpp` generation)
- Original game data files (`東方紅魔郷.exe` v1.02h)

### Building

```bash
# Configure (Win32)
cmake -B build_sdl2 -A Win32

# Build
cmake --build build_sdl2 --config Release --target th06
```

SDL2 libraries are bundled in `3rdparty/` and linked automatically.

### Running

The executable needs access to the original game data. Place the game data files (or `build/` directory from the original game) alongside the executable, or run from the game data directory:

```bash
cd <path-to-game-data>
<path-to>/build_sdl2/Release/th06.exe
```

### Project Structure

```
CMakeLists.txt              # SDL2 build system
3rdparty/
  SDL2/                     # SDL2 development libraries
  SDL2_image/               # SDL2_image development libraries
  SDL2_mixer/               # SDL2_mixer development libraries
  imgui/                    # Dear ImGui (debug overlay)
src/
  sdl2_renderer.cpp/.hpp    # OpenGL rendering backend
  sdl2_compat.hpp           # SDL2/D3D8 compatibility layer
  *.cpp / *.hpp             # Game source (ported from D3D8)
scripts/                    # Original build & tooling scripts
config/                     # Decompilation mapping data
```

### Credits

- [happyhavoc/th06](https://github.com/happyhavoc/th06) — Original reverse-engineered source code
- [Team Shanghai Alice](https://www16.big.or.jp/~zun/) — Original game by ZUN
- [SDL2](https://www.libsdl.org/) / [OpenGL](https://www.opengl.org/)

---

<a id="中文"></a>

## 中文

### 项目简介

本仓库是 [東方紅魔郷 ～ the Embodiment of Scarlet Devil](https://en.touhouwiki.net/wiki/Embodiment_of_Scarlet_Devil) v1.02h 的 **SDL2/OpenGL 移植版**，基于社区 [逆向重建的源代码](https://github.com/happyhavoc/th06)。

原项目通过逆向工程从二进制文件重建游戏源代码，目标是与原版可执行文件逐字节一致。本仓库将 Direct3D 8 / Win32 后端替换为 **SDL2 + OpenGL**，以实现跨平台兼容。

### 特性

- **SDL2 + OpenGL** 渲染后端（替代 Direct3D 8）
- **SDL2_mixer** 音频播放（替代 DirectSound）
- **SDL2_image** 纹理加载
- **CMake** 构建系统
- 已支持 Windows x86/x64（其他平台计划中）

### 环境要求

- CMake >= 3.20
- MSVC（Visual Studio 2019+）或兼容的 C++17 编译器
- Python 3（用于生成 `i18n.hpp`）
- 原版游戏数据文件（`東方紅魔郷.exe` v1.02h）

### 构建

```bash
# 配置（Win32）
cmake -B build_sdl2 -A Win32

# 编译
cmake --build build_sdl2 --config Release --target th06
```

SDL2 库已内置于 `3rdparty/` 目录，自动链接。

### 运行

可执行文件需要访问原版游戏数据。将游戏数据文件（或原版游戏的 `build/` 目录）放在可执行文件旁，或从游戏数据目录运行：

```bash
cd <游戏数据路径>
<路径>/build_sdl2/Release/th06.exe
```

### 项目结构

```
CMakeLists.txt              # SDL2 构建系统
3rdparty/
  SDL2/                     # SDL2 开发库
  SDL2_image/               # SDL2_image 开发库
  SDL2_mixer/               # SDL2_mixer 开发库
  imgui/                    # Dear ImGui（调试覆盖层）
src/
  sdl2_renderer.cpp/.hpp    # OpenGL 渲染后端
  sdl2_compat.hpp           # SDL2/D3D8 兼容层
  *.cpp / *.hpp             # 游戏源码（从 D3D8 移植）
scripts/                    # 原始构建和工具脚本
config/                     # 反编译映射数据
```

### 致谢

- [happyhavoc/th06](https://github.com/happyhavoc/th06) — 原始逆向重建源码
- [上海爱丽丝幻乐团](https://www16.big.or.jp/~zun/) — ZUN 制作的原版游戏
- [SDL2](https://www.libsdl.org/) / [OpenGL](https://www.opengl.org/)
