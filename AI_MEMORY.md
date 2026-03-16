# AI_MEMORY — thprac 集成到 th06_sdl2 长期记忆

## 项目目标
将 thprac（东方 Practice Tool）的核心能力以内置方式集成到 th06_sdl2（东方红魔乡 SDL2/OpenGL 移植版），不破坏现有 SDL2/OpenGL 渲染和跨平台支持。

## th06_sdl2 架构概况
- 项目路径：`c:\Users\13557\source\repos\th06`
- 构建：CMake → `cmake --build build_sdl2 --config Release --target th06`
- 运行：`build_sdl2\Release\th06.exe`，工作目录 `build/`
- 渲染：SDL2 + OpenGL（替代原版 DirectX 8）
- 平台：Windows（主要）、Linux（实验性，build_linux/）
- 文本：GDI TextOut → A8R8G8B8 staging buffer → OpenGL texture
- 音频：SDL2_mixer（WAV）、MidiOutput（MIDI）
- 输入：SDL2 事件 + SDL_GameController
- 已有 ImGui 第三方库在 `3rdparty/imgui/`

## th06_sdl2 核心模块
| 模块 | 文件 | 职责 |
|------|------|------|
| 主循环 | main.cpp | 初始化→GameWindow→Supervisor chain→渲染循环→重启/退出 |
| Supervisor | Supervisor.cpp/hpp | 全局管理器，状态机(INIT/MAINMENU/GAMEMANAGER/...)，PBG3加载，音乐，配置 |
| GameManager | GameManager.cpp/hpp | 游戏核心状态（分数、残机、bomb、power、rank、stage等） |
| Chain | Chain.cpp/hpp | 链式回调系统，OnUpdate/OnDraw/Added/Deleted |
| MainMenu | MainMenu.cpp/hpp | 标题菜单、Practice Start、Replay 等 |
| EclManager | EclManager.cpp/hpp | ECL 脚本执行 |
| BulletManager | BulletManager.cpp/hpp | 弹幕管理 |
| EnemyManager | EnemyManager.cpp/hpp | 敌机管理 |
| Player | Player.cpp/hpp | 自机 |
| Controller | Controller.cpp/hpp | 输入 |
| sdl2_renderer | sdl2_renderer.cpp/hpp | SDL2/OpenGL 渲染后端 |
| ReplayManager | ReplayManager.cpp/hpp | 录像 |
| ResultScreen | ResultScreen.cpp/hpp | 结算画面 |
| Stage | Stage.cpp/hpp | 关卡管理 |
| TextHelper | TextHelper.cpp/hpp | 文本渲染 |
| Gui | Gui.cpp/hpp | 游戏内 UI |

## th06_sdl2 游戏循环
```
main() → Supervisor::RegisterChain() → while(!isAppClosing) {
    GameWindow_ProcessEvents()  // SDL事件
    g_GameWindow.Render()       // RunCalcChain() + RunDrawChain()
}
```
Supervisor::OnUpdate 状态机驱动 wantedState → curState 转换。

## th06_sdl2 关键全局符号
- `g_GameManager` — GameManager 实例
- `g_Supervisor` — Supervisor 实例
- `g_CurFrameInput` / `g_LastFrameInput` — 输入状态
- `g_GameWindow` — 窗口/渲染
- `g_Chain` — 链管理器
- `g_AnmManager` — ANM 动画管理器

## thprac TH06 模块概况
- 源码：`C:\Users\13557\source\repos\thprac\thprac\src\thprac\thprac_th06.cpp` (3740行)
- 头文件：`thprac_th06.h`（GameManager 镜像结构体定义）
- 渲染：Dear ImGui over DX8（需改为 ImGui over OpenGL）
- 输入：VEH hook + 直接内存读取（需改为读 th06_sdl2 全局变量）
- Hook 系统：EHOOK（VEH INT3 断点）+ PATCH（内存字节补丁）— **在内置模式下完全不需要**

## thprac 核心功能与集成优先级
| 功能 | 优先级 | 依赖 | 集成难度 |
|------|--------|------|----------|
| 练习模式菜单(THGuiPrac) | P0 | ImGui, GameManager状态 | 中 |
| 暂停菜单替换(THPauseMenu) | P0 | ImGui, 游戏暂停流程 | 中 |
| 叠加层(THOverlay) | P1 | ImGui, 热键系统 | 低 |
| 游戏内信息(TH06InGameInfo) | P1 | ImGui, spell/miss/bomb事件 | 中 |
| 符卡统计(TH06Save) | P1 | 文件I/O, spell事件 | 低 |
| ECL补丁系统(THPatch) | P0 | ECL字节码访问 | 高 |
| 判定框可视化 | P2 | ImGui渲染, 弹幕数组访问 | 中 |
| Replay参数存储 | P2 | ReplayManager | 中 |
| Cheat热键(F1~F7) | P2 | 直接变量修改 | 低 |
| 高级选项(THAdvOptWnd) | P3 | ImGui | 低 |

## 关键集成决策（初步）
1. **Hook系统不需要移植** — 因为是内置，可以在对应代码点直接调用
2. **ImGui 后端需要 OpenGL** — th06_sdl2 已有 imgui 库，需用 imgui_impl_sdl2 + imgui_impl_opengl2/3
3. **内存地址全部替换为符号引用** — thprac 通过硬编码地址读写，内置版直接引用 `g_GameManager` 等全局变量
4. **ECL 补丁** — 最复杂部分，thprac 通过地址偏移写 ECL 字节码。th06_sdl2 有 EclManager，需确认 ECL 内存布局是否一致
5. **DX8 特有功能** — 判定框贴图(D3DXCreateTextureFromResource)需改为 OpenGL 贴图加载

## 已踩的坑
- ReleasePbg3() 有内建双重释放问题，不能在 restart 路径调用 DeletedCallback
- MainMenu::BeginStartup() 有3秒忙等待，需 SDL_PumpEvents
- Linux 下 DAT 文件发现需要大小写不敏感 glob
- InvertAlpha A8R8G8B8 分支需要 4倍字节范围
- GL_CLAMP_TO_EDGE 需改为 GL_REPEAT 以支持 UV > 1.0 的精灵
- **UpdateKeyState 双重调用 bug**: `ThpracOverlay::NewFrame()` 和 `Overlay::Update()` 都调用 `UpdateKeyState()`，导致 `IsKeyJustPressed()` 永远为 false（prev 和 cur 在同帧内被覆盖为相同值）。修复：`Overlay::Update()` 中移除重复的 `UpdateKeyState()` 调用。
- **原版 thprac 按键习惯**: BACKSPACE 切换 overlay 显示（不是始终可见），F1-F7 为函数快捷键。练习菜单仅在进入 Practice Start 关卡选择时自动弹出（State(1)），由 EHOOK 在 MainMenu 中触发。

## thprac 集成文件清单
| 文件 | 用途 |
|------|------|
| `src/thprac_types.hpp` | PracParam、InGameInfo、OverlayState、PauseMenuState、SaveData 等核心类型 |
| `src/thprac_eclhelper.hpp` | ECLHelper 类，ECLWarp/ECLJump/ECLSetHealth 等 inline 函数 |
| `src/thprac_gui.hpp` / `src/thprac_gui.cpp` | GuiInputInt/GuiCombo/GuiSliderInt 包装 + SDL 键盘状态跟踪 |
| `src/thprac_practice.hpp` / `src/thprac_practice.cpp` | PracticeMenu(State机)、PauseMenu、Overlay(BACKSPACE 面板+F键cheat)、API 接口 |
| `src/thprac_overlay.cpp` | ImGui 叠加层：Init/Shutdown/ProcessEvent/NewFrame/Render |
| `src/thprac_patches.cpp` | ECL 补丁 Stage 1-3 + StageWarp |
| `src/thprac_patches2.cpp` | ECL 补丁 Stage 4-5 |
| `src/thprac_patches3.cpp` | ECL 补丁 Stage 6 |
| `src/thprac_patches4.cpp` | ECL 补丁 Extra |
| `src/thprac_save.cpp` | SaveData 二进制读写 |

## 集成挂钩点
| 挂钩位置 | 文件 | 用途 |
|----------|------|------|
| `GameWindow::InitD3dRendering` | GameWindow.cpp | `ThpracOverlay::Init()` → `thprac::InitThprac()` |
| `GameWindow::Render` | GameWindow.cpp | `ThpracOverlay::NewFrame()` → `ThpracOverlay::Render()` (游戏绘制后、EndScene 前) |
| `GameWindow_ProcessEvents` | GameWindow.cpp | `ThpracOverlay::ProcessEvent()` SDL 事件转发 |
| `GameManager::AddedCallback` | GameManager.cpp | `thprac::OnEnterStage()` |
| `GameManager::DeletedCallback` | GameManager.cpp | `thprac::OnExitStage()` |
| `MainMenu::OnUpdate STATE_PRACTICE_LVL_SELECT` | MainMenu.cpp | `thprac::g_PracticeMenu.State(1/3/4)` 练习菜单触发 |

## 平台相关
- Windows：主开发平台，MSVC 编译
- Linux/WSL2：实验性，GCC 32位，文本渲染空白（需 SDL_ttf）
- Android：尚未支持
