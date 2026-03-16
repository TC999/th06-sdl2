# AI_STATUS — 当前施工状态

## 当前阶段：thprac 核心功能已集成，UI 显示修复完成

### 当前正在做什么
- 修复了 overlay 不显示的根本原因（UpdateKeyState双重调用bug）
- 完成了 Practice Menu 在 MainMenu 中的触发挂钩
- 下一步：暂停菜单增强 + 实际运行验证

### 上一步完成了什么
1. **修复 UpdateKeyState 双重调用 bug**（关键）: `ThpracOverlay::NewFrame()` 已调用 `UpdateKeyState()`，`Overlay::Update()` 又调用了一次，导致 `IsKeyJustPressed()` 永远返回 false。移除重复调用后 BACKSPACE 和 F1-F7 热键恢复工作。
2. **Overlay 改为原版 thprac 风格**: 位置 (10,10)，自动大小，无标题栏，显示 checkbox 切换（Invincible/InfLives/InfBombs/InfPower/FrameCounter）
3. **MainMenu 挂钩**: STATE_PRACTICE_LVL_SELECT 中添加 State(1/3/4) 调用，进入练习关卡选择时自动弹出练习菜单，确认时填充 PracParam 并覆盖关卡选择，取消时关闭
4. **PracticeMenu::State() 方法**: 实现状态机，与原版 THGuiPrac::State() 行为一致

### 已完成的集成工作
- 全部 ECL 补丁文件（thprac_patches.cpp ~ thprac_patches4.cpp，覆盖 Stage 1-7/Extra）
- thprac_types.hpp / thprac_eclhelper.hpp / thprac_gui.hpp+cpp / thprac_practice.hpp+cpp
- thprac_overlay.cpp（ImGui SDL2+OpenGL2 渲染管线）
- thprac_save.cpp（SaveData 二进制读写）
- GameWindow.cpp / GameManager.cpp / MainMenu.cpp 挂钩

### 下一步准备做什么
1. **用户运行验证**: BACKSPACE 显示 overlay、进入 Practice Start 显示练习菜单
2. **暂停菜单增强**: 在 GameManager::OnUpdate 的 isInGameMenu 处理中，当 thPracParam.mode 有效时替换为 thprac 暂停菜单（Resume/Restart/Settings）
3. **Cheat 效果完善**: 当前无敌模式直接设 playerState，可能有副作用，需改为更细粒度控制

### 当前阻塞点
- 无

### 当前涉及文件
- src/thprac_practice.cpp（核心修改：修复 UpdateKeyState、Overlay 样式、State()）
- src/MainMenu.cpp（添加 thprac 练习菜单挂钩）
- src/thprac_practice.hpp（添加 State() 声明）

### 当前测试状态
- Windows Release 构建：✅ 编译通过，零错误
- 运行验证：⏳ 等待用户测试

### 当前是否可编译/可运行/可验证
- 可编译：✅
- 可运行：✅
- 可验证：✅（BACKSPACE 开关 overlay，Practice Start 弹出菜单）
