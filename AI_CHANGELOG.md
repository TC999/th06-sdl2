# AI_CHANGELOG — 改动日志

## [2026-03-15] 修复 overlay 不显示 + Practice Menu MainMenu 挂钩
- 目标：修复"游戏里什么都没有"问题，让 thprac overlay 和练习菜单实际可见
- 修改文件：src/thprac_practice.cpp, src/thprac_practice.hpp, src/MainMenu.cpp
- 修改原因：
  1. UpdateKeyState() 在 NewFrame 和 Overlay::Update 中双重调用，导致 IsKeyJustPressed 永远返回 false（prev=cur）
  2. Overlay::Draw() 样式与原版 thprac 不一致（大窗口+文字信息 vs 小面板+checkbox）
  3. PracticeMenu 没有触发入口（isOpen 永远为 false）
- 修改内容：
  1. **thprac_practice.cpp Overlay::Update()**: 移除重复的 UpdateKeyState() 调用。ThpracOverlay::NewFrame() 已经调用过一次。
  2. **thprac_practice.cpp Overlay::Draw()**: 改为 (10,10) 位置、NoTitleBar/AutoResize/NoMove 风格，显示 checkbox 控件（Invincible/InfLives/InfBombs/InfPower/FrameCounter），与原版 THOverlay 一致。
  3. **thprac_practice.cpp PracticeMenu::Draw()**: 改为固定位置 (260,65) 大小 (330,390)，匹配原版 THGuiPrac 布局。移除 "Apply & Start" 按钮（由 State(3) 控制）。
  4. **thprac_practice.cpp PracticeMenu::State()**: 新增状态机方法。State(1)=打开+读取难度，State(3)=填充PracParam+关闭，State(4)=关闭。
  5. **thprac_practice.hpp**: 添加 State(int) 声明。
  6. **MainMenu.cpp**: include thprac_practice.hpp。STATE_PRACTICE_LVL_SELECT 中添加：stateTimer==0 时 State(1)，RETURNMENU 时 State(4)，SELECTMENU 时 State(3) 并用 thprac 的 stage 覆盖 currentStage。
- 风险：中。MainMenu 修改影响练习模式流程；如果 thprac stage 选择越界可能导致关卡加载错误。
- 验证方式：Release 构建 + 运行（BACKSPACE 显示 overlay，进入 Practice Start 显示菜单）
- 验证结果：✅ 编译成功（零错误）
- 下一步：用户运行验证；暂停菜单增强；cheat 效果完善

## [2026-03-15] thprac 核心功能文件批量创建
- 目标：创建所有 thprac 集成文件（类型定义、ECL helper、GUI、练习菜单、ECL补丁、保存系统）
- 修改文件：新建 src/thprac_types.hpp, thprac_eclhelper.hpp, thprac_gui.hpp/cpp, thprac_practice.hpp/cpp, thprac_patches.cpp, thprac_patches2/3/4.cpp, thprac_save.cpp。修改 CMakeLists.txt, GameManager.cpp, GameWindow.cpp。
- 修改原因：将 thprac_th06.cpp (3740行) 的核心能力拆分为独立模块集成
- 修改内容：
  1. 类型定义：PracParam, InGameInfo, OverlayState, PauseMenuState, SaveData
  2. ECL补丁：7个关卡全部sections的ECL字节码补丁（使用 g_EclManager.eclFile 替代硬编码地址）
  3. GUI包装：GuiInputInt/GuiCombo/GuiSliderInt/GuiCheckbox + SDL键盘状态跟踪
  4. 集成挂钩：GameManager OnEnterStage/OnExitStage, GameWindow InitThprac
- 风险：中。ECL补丁字节码偏移需要与实际ECL文件匹配。
- 验证结果：✅ Release 构建零错误

## [2026-03-14 17:40] ImGui 叠加层基础集成
- 目标：将 Dear ImGui (SDL2 + OpenGL2 后端) 集成到 th06_sdl2 渲染管线
- 修改文件：CMakeLists.txt, src/thprac_overlay.hpp(新建), src/thprac_overlay.cpp(新建), src/GameWindow.cpp, src/main.cpp
- 修改原因：thprac 所有 GUI 功能基于 ImGui，必须先建立 ImGui 渲染通道
- 修改内容：
  1. CMakeLists.txt: 添加 imgui 核心(4文件) + SDL2后端 + OpenGL2后端 + thprac_overlay.cpp 到编译列表；添加 3rdparty/imgui 到 include 目录
  2. thprac_overlay.hpp/cpp: 封装 ImGui 初始化(Init)、事件转发(ProcessEvent)、帧更新(NewFrame)、渲染(Render)、清理(Shutdown) 接口。含一个测试窗口显示 "thprac overlay OK"
  3. GameWindow.cpp: InitD3dRendering() 中 GL context 创建后调用 ThpracOverlay::Init()；ProcessEvents() 中将 SDL 事件传递给 ImGui；Render() 中在 RunDrawChain 后 EndScene 前渲染 ImGui（绘制到 FBO）
  4. main.cpp: 退出路径添加 ThpracOverlay::Shutdown()
- 风险：低。ImGui 渲染是追加操作，不修改现有绘制逻辑。GL 状态 save/restore 在 Render() 中处理。
- 验证方式：Release 构建 + 运行
- 验证结果：✅ 编译成功（无错误，仅 C4819 编码警告）；✅ 运行正常（进程 Responding=True，未崩溃）
- 下一步：目视确认 ImGui 测试窗口可见；然后开始实现 BACKSPACE 叠加层菜单（THOverlay）
