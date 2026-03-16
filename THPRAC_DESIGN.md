# thprac 练习工具完整移植设计文档

> 基于 RUEEE 魔改版 thprac (`thprac_th06.cpp`, 151KB, ~3700行)  
> 目标项目: th06_sdl2 源码版 (SDL2 + OpenGL2 + ImGui)  
> 文档精确到代码级别

---

## 1. 架构概述

### 1.1 原版 thprac 架构 (DLL注入 + 内存补丁)

```
┌─────────────────────────────────────────────────┐
│  thprac DLL (注入到 th06e.exe 进程)              │
│                                                  │
│  EHOOK_DY(addr, size, { ... })                  │
│    → 在游戏指定地址插入跳转到回调函数             │
│    → 回调可修改寄存器(pCtx->Eax等)和内存          │
│                                                  │
│  PATCH_HK(addr, "bytes")                        │
│    → 热补丁: 替换指定地址的机器码字节             │
│                                                  │
│  SINGLETON(ClassName)                            │
│    → 懒加载单例: static s_singleton = new ...    │
│                                                  │
│  GameGuiWnd 基类                                 │
│    → 管理 ImGui 窗口的 Open/Close/Update/Fade    │
│    → mStatus: 0=关闭 1=打开中 2=已打开 3=关闭中   │
│                                                  │
│  渲染后端: IMPL_WIN32_DX8 (IDirect3DDevice8)     │
│  输入后端: Win32 VK_ 虚拟键码                     │
└─────────────────────────────────────────────────┘
```

### 1.2 th06_sdl2 源码版架构 (直接源码修改)

```
┌─────────────────────────────────────────────────┐
│  th06_sdl2 (源码编译)                            │
│                                                  │
│  无需 EHOOK: 直接在对应函数中调用                 │
│    → th06_prac_menu_1 (0x437179)                │
│      = MainMenu 练习模式选择后调用               │
│    → th06_patch_main (0x41c17a)                  │
│      = GameManager::AddedCallback() 关卡初始化时  │
│    → th06_pause_menu (0x401b8f)                  │
│      = Supervisor 暂停菜单逻辑中                  │
│    → th06_update (0x41caac)                      │
│      = Supervisor::TickCallback() 每帧更新        │
│    → th06_render (0x41cb6d)                      │
│      = Supervisor::DrawCallback() 每帧渲染        │
│                                                  │
│  无需 PATCH_HK: 直接修改变量值                    │
│    → mMuteki 的 PATCH_HK(0x4277c2)              │
│      = 源码中设 g_Player.bulletGracePeriod = 60  │
│                                                  │
│  渲染后端: SDL2 + OpenGL2 + ImGui                │
│  输入后端: SDL_Scancode                           │
└─────────────────────────────────────────────────┘
```

### 1.3 地址→源码符号映射表

EHOOK 和内存读写中使用的硬编码地址，与 th06_sdl2 源码符号的对应关系:

| 地址 | 类型 | thprac 用法 | th06_sdl2 源码等价物 |
|------|------|-------------|---------------------|
| `0x69bcb0` | int8 | 难度 | `g_GameManager.difficulty` |
| `0x69d4ba` | int8 | 残机数 | `g_GameManager.livesRemaining` |
| `0x69d4bb` | int8 | Bomb数 | `g_GameManager.bombsRemaining` |
| `0x69d4b0` | int16 | 火力 | `g_GameManager.currentPower` |
| `0x69bca0` | int32 | GUI分数 | `g_GameManager.guiScore` |
| `0x69bca4` | int32 | 实际分数 | `g_GameManager.score` |
| `0x69bcb4` | int32 | 擦弹 | `g_GameManager.grazeInStage` |
| `0x69bcb8` | int32 | 擦弹(总计) | `g_GameManager.grazeTotal` |
| `0x69d4b4` | int16 | 点数(关卡) | `g_GameManager.pointItemsCollectedInStage` |
| `0x69d4b6` | int16 | 点数(总计) | `g_GameManager.pointItemsCollected` |
| `0x69d710` | int32 | Rank | `g_GameManager.rank` |
| `0x69d714` | int32 | Rank上限 | `g_GameManager.maxRank` |
| `0x69d718` | int32 | Rank下限 | `g_GameManager.minRank` |
| `0x69d71c` | int32 | 子Rank | `g_GameManager.subRank` |
| `0x69d4bd` | int8 | 自机(0=灵梦,1=魔理沙) | `g_GameManager.character` |
| `0x69d4be` | int8 | 弹型(0=A,1=B) | `g_GameManager.shotType` |
| `0x69d4bc` | int8 | Extend进度 | `g_GameManager.extraLives` |
| `0x69d4bf` | byte | 暂停菜单状态 | `g_GameManager.pauseMenuState` |
| `0x69bcbc` | byte | 是否Replay | `g_GameManager.isInReplay` |
| `0x69d6d4` | int32 | 当前关卡 | `g_GameManager.currentStage` |
| `0x69d6d8` | int32 | 关卡(备份) | `g_GameManager.nextStage` |
| `0x6c6ea4` | int32 | 游戏状态 | `g_Supervisor.curState` |
| `0x6c6eb0` | int32 | 输入掩码 | `g_Supervisor.lockInputMask` |
| `0x5a5fb0` | int32 | ECL时间 | `g_EclManager.eclFile` 偏移 |
| `0x69D904` | WORD | 当前帧输入 | `g_CurFrameInput` |
| `0x69D908` | WORD | 上帧输入 | `g_LastFrameInput` |
| `0x487e50` | void* | ECL数据基址 | `g_EclManager.eclFile` |
| `0x487e44` | int32 | ECL shot类型 | `g_EclManager.shotType` |
| `0x6CAA68` | float | 玩家X | `g_Player.positionCenter.x` |
| `0x6CAA6C` | float | 玩家Y | `g_Player.positionCenter.y` |

### 1.4 输入系统映射

| thprac (Win32) | th06_sdl2 (SDL) | 用途 |
|---------------|-----------------|------|
| `VK_F1` ~ `VK_F8` | `SDL_SCANCODE_F1` ~ `SDL_SCANCODE_F8` | 作弊开关 |
| `VK_ESCAPE` | `SDL_SCANCODE_ESCAPE` | 暂停 |
| `VK_BACK` (BACKSPACE) | `SDL_SCANCODE_BACKSPACE` | 叠加菜单开关 |
| `GetChordPressed(GetAdvancedMenuChord())` | `SDL_SCANCODE_F12` | 高级选项窗口 |
| `'R'` 键 | `SDL_SCANCODE_R` | 快速重开 |
| `'Q'` 键 | `SDL_SCANCODE_Q` | 退出到标题 |
| `KeyboardInputGetSingle(VK_ESCAPE)` | `InGameInputGet(SDL_SCANCODE_ESCAPE)` | 边沿触发ESC |
| `KeyboardInputGetRaw(key)` | `SDL_GetKeyboardState()[sc]` | 电平检测 |

---

## 2. THPracParam — 练习参数结构体

thprac 核心数据结构，GUI填充后由 EHOOK 在关卡初始化时应用到游戏状态。

### 2.1 原版定义 (thprac_th06.cpp:231-330)

```cpp
struct THPracParam {
    int32_t mode;       // 0=正常游戏  1=自定义练习

    int32_t stage;      // 0-5 (Stage 1~6), 6 = Extra
    int32_t section;    // th_sections_t 枚举值，指定 boss/符卡/章节
    int32_t phase;      // 符卡阶段 (用于多阶段符卡如 ST7_END_S10)
    int32_t frame;      // ECL 帧跳转 (高级: 直接跳到某帧)

    int64_t score;      // 初始分数
    float   life;       // 残机数 (0-8)
    float   bomb;       // Bomb数 (0-8)
    float   power;      // 火力 (0-128)
    int32_t graze;      // 擦弹数
    int32_t point;      // 点数

    int32_t rank;       // Rank值 (0-32, rankLock时0-99)
    bool    rankLock;   // 是否锁定Rank
    int32_t fakeType;   // 伪装弹型 (0=不伪装, 1-4=灵A/灵B/魔A/魔B)
                        // 仅 ST4 Boss (帕秋莉) 有效

    int32_t delay_st6bs9; // ST6 Boss9 延迟帧数 (0-600)
    bool    wall_prac_st6; // ST6 墙练开关
    bool    dlg;          // 是否播放对话

    // ST4 Books (帕秋莉六本书位置自定义)
    int bF;             // 书启用标志位 (bit0~bit5 对应6本书)
    int bX1,bX2,bX3,bX4,bX5,bX6; // X坐标 (-192~192)
    int bY1,bY2,bY3,bY4,bY5,bY6; // Y坐标 (-50~448)

    // 狙击练习参数
    int snipeN;         // 近距狙击概率 (0-100%)
    int snipeF;         // 远距狙击概率 (0-100%)

    bool _playLock;     // 防止重复应用参数

    void Reset() { /* 见下 */ }
    bool ReadJson(std::string& json);  // 从replay读取
    std::string GetJson();             // 序列化到replay
};
```

### 2.2 源码版适配

```cpp
// src/ThpracParam.hpp — 新增文件
struct ThpracParam {
    int  mode = 0;

    int  stage = 0;
    int  section = 0;      // th_sections_t
    int  phase = 0;
    int  frame = 0;

    i64  score = 0;
    int  life = 0;
    int  bomb = 0;
    int  power = 0;
    int  graze = 0;
    int  point = 0;

    int  rank = 0;
    bool rankLock = false;
    int  fakeType = 0;

    int  delay_st6bs9 = 0;
    bool wall_prac_st6 = false;
    bool dlg = false;

    int  bF = 0;
    int  bX[6] = {};
    int  bY[6] = {32,128,144,64,80,96};

    int  snipeN = 0;
    int  snipeF = 0;

    bool playLock = false;

    void Reset();
};

extern ThpracParam g_thPracParam;
```

> **适配要点**: 移除 JSON 序列化（th06_sdl2 的 replay 格式不同）；`float life/bomb/power` 改为 `int`（赋值时转换即可）。

---

## 3. Section 枚举 — 选关系统

### 3.1 th_sections_t (thprac_locale_def.h:1840-1947)

107 个枚举值，定义了 TH06 所有可练习段落：

```cpp
enum th_sections_t {
    // Stage 1
    TH06_ST1_MID1,    // 道中BOSS (大妖精)
    TH06_ST1_BOSS1,   // Boss (露米娅) 非符1
    TH06_ST1_BOSS2,   // 月符「月光」
    TH06_ST1_BOSS3,   // 夜符「暗夜鸟」
    TH06_ST1_BOSS4,   // H/L 夜符「夜雀」

    // Stage 2
    TH06_ST2_MID1,
    TH06_ST2_BOSS1,   // Boss (琪露诺)
    TH06_ST2_BOSS2,   // 冰符「アイシクルフォール」
    TH06_ST2_BOSS3,   // ... (省略部分)
    // ...

    // Stage 4 特殊: 帕秋莉根据弹型不同有不同符卡
    TH06_ST4_BOSS1,   // Boss (帕秋莉) 非符1
    TH06_ST4_BOSS2,   // 火符 / 土符 / 水符 / 木符 (按弹型)
    // ... 每个弹型有独立符卡序列
    TH06_ST4_BOOKS,   // 六本书特殊练习模式

    // Stage 6
    TH06_ST6_BOSS6,   // 蕾米Boss6 (墙练目标之一)
    TH06_ST6_BOSS9,   // 最终符卡 (墙练目标之二)

    // Extra
    TH06_ST7_MID1,    // 道中Boss
    TH06_ST7_END_NS1, // Boss (芙兰) 非符1
    TH06_ST7_END_S10, // 最终符卡「レーヴァテイン」(多阶段)
    // ...
    // 共 107 项
};
```

### 3.2 Warp 模式 (mWarp 下拉框)

```
mWarp = 0: 无跳转 (从关卡开头开始)
mWarp = 1: 章节跳转 (Chapter)
            → mChapter 选择具体章节
            → section = 10000 + (stage+1)*100 + chapter
mWarp = 2: 道中Boss (Mid boss)
            → mSection 从 th_sections_cba[stage][0][i] 选择
mWarp = 3: 关底Boss (End boss)
            → mSection 从 th_sections_cba[stage][1][i] 选择
mWarp = 4: 非符 (Non-spell)
            → mSection 从 th_sections_cbt[stage][0][i] 选择
mWarp = 5: 符卡 (Spellcard)
            → mSection 从 th_sections_cbt[stage][1][i] 选择
mWarp = 6: 帧跳转 (Frame)
            → mFrame 输入具体帧号
```

### 3.3 查找表

```cpp
// 索引: [stage + st][boss_type][spell_index]
// st = Stage4时根据fakeType偏移4-7
extern const int th_sections_cba[11][2][19]; // boss型 (mid/end)
extern const int th_sections_cbt[11][2][14]; // 类型  (非符/符卡)
extern const char* th_sections_str[3][4][107]; // [语言][难度][section] → 显示名
extern const int th_sections_bgm[107]; // section → BGM编号

// 每关章节数: [stage][0]=前半, [1]=后半
static int mChapterSetup[7][2] = {
    {4,2}, {2,2}, {4,3}, {4,5}, {3,2}, {2,0}, {4,3}
};
```

---

## 4. BKSP 叠加菜单 (THOverlay)

### 4.1 原版 THOverlay (thprac_th06.cpp:369-475)

```cpp
class THOverlay : public Gui::GameGuiWnd {
    THOverlay() noexcept {
        SetTitle("Mod Menu");
        SetFade(0.5f, 0.5f);  // 半透明
        SetPos(10.0f, 10.0f); // 左上角
        SetSize(0.0f, 0.0f);  // 自动大小
        SetWndFlag(
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav);
    }
    SINGLETON(THOverlay)

    // BACKSPACE 和弦 → 开关叠加窗口
    Gui::GuiHotKeyChord mMenu { "ModMenuToggle", "BACKSPACE", ... };

    // F1-F8 热键 + 内存补丁:
    HOTKEY_DEFINE(mMuteki, TH_MUTEKI, "F1", VK_F1)
        PATCH_HK(0x4277c2, "03"),          // 碰撞判定 → 无敌
        PATCH_HK(0x42779a, "83c4109090")   // 跳过伤害处理
    HOTKEY_ENDDEF();

    HOTKEY_DEFINE(mInfLives, TH_INFLIVES2, "F2", VK_F2)
        PATCH_HK(0x428DDB, "a0bad469009090909090909090909090"),
        PATCH_HK(0x428AC6, "909090909090")
    HOTKEY_ENDDEF();

    HOTKEY_DEFINE(mInfBombs, TH_INFBOMBS, "F3", VK_F3)
        PATCH_HK(0x4289e3, "00")
    HOTKEY_ENDDEF();

    HOTKEY_DEFINE(mInfPower, TH_INFPOWER, "F4", VK_F4)
        PATCH_HK(0x428B7D, "00"),
        PATCH_HK(0x428B67, "909090909090909090")
    HOTKEY_ENDDEF();

    HOTKEY_DEFINE(mTimeLock, TH_TIMELOCK, "F5", VK_F5)
        PATCH_HK(0x412DD1, "eb")           // rank计时器跳转
    HOTKEY_ENDDEF();

    HOTKEY_DEFINE(mAutoBomb, TH_AUTOBOMB, "F6", VK_F6)
        PATCH_HK(0x428989, "EB1D"),        // 跳过被弹检测
        PATCH_HK(0x4289B4, "85D2"),        // bomb条件改为始终有效
        PATCH_HK(0x428A94, "FF89"),
        PATCH_HK(0x428A9D, "66C70504D9690002")
    HOTKEY_ENDDEF();

    Gui::GuiHotKey mElBgm { TH_EL_BGM, "F7", VK_F7 };
    Gui::GuiHotKey mShowSpellCapture { THPRAC_INGAMEINFO, "F8", VK_F8 };

    // OnContentUpdate: 每帧渲染8行热键状态
    void OnContentUpdate() override {
        mMuteki();    // 渲染一行: checkbox + 彩色标签
        mInfLives();
        mInfBombs();
        mInfPower();
        mTimeLock();
        mAutoBomb();
        mElBgm();
        mShowSpellCapture();
    }
};
```

### 4.2 源码版适配方案

**关键变化**: 不使用 PATCH_HK 内存补丁，改为源码级变量控制。

```cpp
// 每个 PATCH_HK 的源码等价实现 (在 ApplyCheats() 中):
struct CheatEffects {
    // F1 mMuteki: PATCH_HK(0x4277c2) → 源码:
    if (invincible && g_Player.bulletGracePeriod < 60)
        g_Player.bulletGracePeriod = 60;
    // 注: 原版是修改碰撞判定指令让碰撞永远不生效
    // 源码版是保持无敌帧 ≥ 60，效果等价

    // F2 mInfLives: PATCH_HK(0x428DDB) → 源码:
    if (infLives && g_GameManager.livesRemaining < 2)
        g_GameManager.livesRemaining = 2;
    // 原版是NOP掉减命指令; 源码版是每帧检查并恢复

    // F3 mInfBombs: PATCH_HK(0x4289e3) → 源码:
    if (infBombs && g_GameManager.bombsRemaining < 3)
        g_GameManager.bombsRemaining = 3;

    // F4 mInfPower: PATCH_HK(0x428B7D) → 源码:
    if (infPower && g_GameManager.currentPower < 128)
        g_GameManager.currentPower = 128;

    // F5 mTimeLock: PATCH_HK(0x412DD1) → 源码:
    if (timeLock)
        g_GameManager.isTimeStopped = 1;

    // F6 mAutoBomb: PATCH_HK(0x428989/0x4289B4/0x428A94/0x428A9D) → 源码:
    // 需要在 Player::HandleDeath() 中:
    //   当被弹时自动消耗bomb而非死亡
    //   修改 Player 的被弹分支判定

    // F7 mElBgm: 永续BGM → 暂停时不停止BGM播放
    // 需要在 MidiOutput / SoundManager 暂停逻辑中跳过

    // F8 mShowSpellCapture: 显示游戏信息叠加层
    // 纯UI，无需游戏逻辑修改
};
```

---

## 5. 练习菜单 (THGuiPrac)

### 5.1 窗口配置 (thprac_th06.cpp:476-500)

```cpp
THGuiPrac() noexcept {
    *mLife = 8; *mBomb = 8; *mPower = 128;
    *mMode = 1; *mScore = 0; *mGraze = 0; *mRank = 32;
    SetFade(0.8f, 0.1f);
    SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
    SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
}
// ZH_CN locale:
SetSize(330.f, 390.f);
SetPos(260.f, 65.f);
SetItemWidth(-60.0f);
```

### 5.2 状态机 (State)

```
State(0): 无操作
State(1): 打开菜单 → 读取当前游戏状态(难度/弹型)
    触发位置: th06_prac_menu_1 EHOOK (0x437179)
    源码等价: MainMenu 中选择"练习开始"后调用

State(3): 关闭菜单 → 填充 thPracParam → 开始游戏
    触发位置: th06_prac_menu_3 EHOOK (0x43738c)
    源码等价: MainMenu 确认选择后调用
    动作:
      thPracParam.mode = *mMode;
      thPracParam.stage = *mStage;
      thPracParam.section = CalcSection();
      thPracParam.phase = *mPhase;
      thPracParam.frame = *mFrame;
      if (SectionHasDlg(section)) thPracParam.dlg = *mDlg;
      // ST6 Boss9 特殊参数
      if (section == TH06_ST6_BOSS9) thPracParam.delay_st6bs9 = *mDelaySt6Bs9;
      if (section == TH06_ST6_BOSS9 || TH06_ST6_BOSS6) {
          thPracParam.wall_prac_st6 = *mWallPrac;
          thPracParam.snipeF/N = ...;
      }
      // ST4 Books 六本书参数
      if (section == TH06_ST4_BOOKS) {
          thPracParam.bF = (mBookC1<<0) | (mBookC2<<1) | ...;
          thPracParam.bX1~6 = ...; thPracParam.bY1~6 = ...;
      }
      // 基础参数
      thPracParam.score/life/bomb/power/graze/point/rank/rankLock = ...;
      // ST4 Boss 弹型伪装
      if (section >= TH06_ST4_BOSS1 && <= TH06_ST4_BOSS7)
          thPracParam.fakeType = *mFakeShot;

State(4): 关闭菜单 (取消，不填充参数)
    触发位置: th06_prac_menu_4 EHOOK (0x43723f)

State(5): 填充参数 (从暂停菜单的"练习设置"触发)
    与 State(3) 相同的参数填充逻辑
    不关闭窗口，仅更新 thPracParam
```

### 5.3 PracticeMenu UI 渲染

```cpp
void PracticeMenu(Gui::GuiNavFocus& nav_focus) {
    mMode();          // 下拉: "正常游戏" / "自定义练习"
    if (mStage())     // 下拉: Stage 1-6
        *mSection = *mChapter = 0;  // 切换关卡时重置

    if (*mMode == 1) {
        if (mWarp()) {  // 下拉: 无跳转/章节/道中Boss/关底Boss/非符/符卡/帧
            // 重置所有子选项
            *mSection = *mChapter = *mPhase = *mFrame = 0;
            *mDelaySt6Bs9 = 120;
            *mWallPrac = false;
            // 重置书位置到默认
            *mBookC1~6 = true;
            *mBookX1=-180, *mBookY1=32, ...;
        }
        if (*mWarp) {
            int st = 0;
            if (*mStage == 3) {  // Stage 4: 帕秋莉弹型选择
                mFakeShot();     // 下拉: 不伪装/灵A/灵B/魔A/魔B
                st = (*mFakeShot ? *mFakeShot-1 : mShotType) + 4;
            }
            SectionWidget();     // 根据 mWarp 显示对应选择器
            SpellPhase();        // 根据 section 显示阶段选择器
        }

        mLife();      // 滑块: 0-8
        mBomb();      // 滑块: 0-8
        mScore();     // 拖拽: 0-9999999990 (步进10/100000000)
        mScore.RoundDown(10);
        mPower();     // 滑块: 0-128
        mGraze();     // 拖拽: 0-99999
        mPoint();     // 拖拽: 0-9999
        mRank();      // 滑块: 0-32 (锁定时0-99)
        if (mRankLock()) {
            if (*mRankLock) mRank.SetBound(0, 99);
            else            mRank.SetBound(0, 32);
        }
    }
    nav_focus();
}
```

### 5.4 SpellPhase 特殊阶段选择器

```cpp
void SpellPhase() {
    auto section = CalcSection();

    if (section == TH06_ST7_END_S10) {
        // Extra 最终符: 阶段选择 (phase 1-N)
        mPhase(TH_PHASE, TH_SPELL_PHASE1);
    }
    else if (section == TH06_ST4_BOOKS) {
        // 帕秋莉六本书: 阶段选择 + 书位置编辑器
        mPhase(TH_PHASE, TH_BOOKS_PHASE_INF_TIME);
        if (*mPhase == 4) {
            // 3列布局: [启用] [X坐标] [Y坐标] × 6行
            ImGui::Columns(3);
            mBookC1(); mBookX1(); mBookY1(); // 第1本书
            mBookC2(); mBookX2(); mBookY2(); // ...
            // ...共6行
            ImGui::Columns(1);

            // 便捷按钮:
            Button("镜像")     → bX4=-bX3, bX5=-bX2, bX6=-bX1
            Button("全镜像")   → 所有bX取反
            Button("轮转")     → X坐标循环移位
            Button("随机X")    → 随机生成6个X (-192~192)
            Button("重置Y")    → 恢复默认Y {32,128,144,64,80,96}
            Button("复制设置") → 格式化到剪贴板
            Button("粘贴设置") → 从剪贴板解析
        }
    }
    else if (section == TH06_ST5_BOSS6) {
        mPhase(TH_PHASE, TH_EOSD_SAKUYA_DOLLS);
    }
    else if (section == TH06_ST6_BOSS9) {
        // 最终符: 阶段选择 + 延迟 + 墙练
        mPhase(TH_PHASE, TH06_FINAL_SPELL);
        if (*mPhase == 1|2|3) mDelaySt6Bs9(); // 延迟帧输入
        mWallPrac();                            // 墙练开关
        if (*mWallPrac) {
            mWallPracSnipeN("%d%%");            // 近距狙击%
            mWallPracSnipeF("%d%%");            // 远距狙击%
        }
    }
    else if (section == TH06_ST6_BOSS6) {
        mWallPrac();
        if (*mWallPrac) {
            mWallPracSnipeN("%d%%");
            mWallPracSnipeF("%d%%");
        }
    }
}
```

---

## 6. 暂停菜单 (THPauseMenu)

### 6.1 原版 THPauseMenu (thprac_th06.cpp:956-1223)

完全接管游戏原生暂停菜单，替换为带练习设置入口的 ImGui 版本。

```cpp
class THPauseMenu : public Gui::GameGuiWnd {
    THPauseMenu() noexcept {
        SetFade(0.8f, 0.1f);
        SetSize(384.f, 448.f);
        SetPos(32.f, 16.f);
        SetItemWidth(-60);
        SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
        SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    // 状态枚举
    enum state {
        STATE_CLOSE   = 0,
        STATE_RESUME  = 1,
        STATE_EXIT    = 2,
        STATE_RESTART = 3,
        STATE_OPEN    = 4,
        STATE_EXIT2   = 5,
    };
    enum signal {
        SIGNAL_NONE    = 0,
        SIGNAL_RESUME  = 1, // 继续游戏
        SIGNAL_EXIT    = 2, // 退出到结算
        SIGNAL_RESTART = 3, // 重新开始
        SIGNAL_EXIT2   = 4, // 退出到标题
    };
};
```

### 6.2 状态机 PMState()

```
StateClose → (5帧后) → StateOpen
StateOpen  → (10帧后接受输入)
    ESC按下 → StateResume
    Q按下   → StateExit2
    R按下   → StateRestart
StateResume  → (1帧: 关闭窗口) → (10帧: 返回 SIGNAL_RESUME)
StateExit    → (1帧: 关闭窗口) → (10帧: 返回 SIGNAL_EXIT)
StateExit2   → (1帧: 关闭窗口) → (10帧: 返回 SIGNAL_EXIT2)
StateRestart → (1帧: 填充参数+关闭) → (10帧: 返回 SIGNAL_RESTART)
```

StateRestart 的关键逻辑:
```cpp
signal StateRestart() {
    if (mFrameCounter == 1) {
        auto oldMode = thPracParam.mode;
        auto oldStage = thPracParam.stage;
        auto oldBgmFlag = THBGMTest();
        thRestartFlag = true;

        // 从当前练习菜单重新填充参数 (State(5))
        THGuiPrac::singleton().State(5);

        // 判断是否需要保持BGM (永续BGM)
        if (*(THOverlay::singleton().mElBgm)
            && !el_bgm_changed
            && oldMode == thPracParam.mode
            && oldStage == thPracParam.stage
            && oldBgmFlag == THBGMTest()) {
            el_bgm_signal = true;
        }
        Close();
    } else if (mFrameCounter == 10) {
        return SIGNAL_RESTART;
    }
}
```

### 6.3 暂停菜单 UI

```cpp
void OnContentUpdate() override {
    if (!inSettings) {
        // 正常暂停菜单布局
        ImGui::Dummy(ImVec2(10.0f, 140.0f));
        ImGui::Indent(119.0f);
        mResume();     // "继续" 按钮 (130×25)
        mExit();       // "退出" 按钮
        mExit2();      // "返回标题" 按钮
        mRestart();    // "重新开始" 按钮
        mSettings();   // "练习设置" 按钮 → 切换 inSettings
        ImGui::Unindent();
        mNavFocus();

        // 上键循环: Resume 上方的焦点跳到 Settings
        // 下键循环: Settings 下方的焦点跳到 Resume
    } else {
        // 练习设置模式: 按钮缩小 + 显示练习菜单
        ImGui::Dummy(ImVec2(10.0f, 10.0f));  // 减小顶部间距
        ImGui::Indent(119.0f);
        mResume(); mExit(); mExit2(); mRestart(); mSettings();
        ImGui::Unindent(67.0f);
        // 嵌入练习菜单
        THGuiPrac::singleton().PracticeMenu(mNavFocus);
    }
}
```

### 6.4 EHOOK 触发 (th06_pause_menu, 0x401b8f)

```cpp
// 源码等价位置: Supervisor::TickCallback() 中暂停逻辑
if (thPracParam.mode && !g_GameManager.isInReplay) {
    auto sig = THPauseMenu::singleton().PMState();
    switch (sig) {
        case SIGNAL_RESUME:
            // 跳转到恢复游戏
            break;
        case SIGNAL_EXIT:
            g_Supervisor.curState = 7; // 结算画面
            g_GameManager.pauseMenuState = 0;
            break;
        case SIGNAL_RESTART:
            // 跳转到重启流程
            break;
        case SIGNAL_EXIT2:
            g_Supervisor.curState = 1; // 返回标题
            g_GameManager.pauseMenuState = 0;
            break;
        default:
            // 阻断原生暂停菜单逻辑
            break;
    }
}

// escR 补丁 (非练习模式也可用):
// R键 或 Ctrl+Shift+↓ → 快速重开
if (!thPracParam.mode && !g_GameManager.isInReplay) {
    if (KeyPressed('R') || (input & 0x124) == 0x124) {
        threstartflag_normalgame = true;
        // 跳转到重启流程
    }
}
```

---

## 7. F12 高级选项 (THAdvOptWnd)

### 7.1 原版 THAdvOptWnd (thprac_th06.cpp:1425-1775)

全屏覆盖窗口，F12 开关。原版通过 `Gui::PPGuiWnd` 回调式基类。

```cpp
class THAdvOptWnd : public Gui::PPGuiWnd {
    THAdvOptWnd() noexcept {
        SetWndFlag(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        SetFade(0.8f, 0.8f);
        SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
        SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
        // 全屏:
        SetSizeRel(1.0f, 1.0f);
        SetPosRel(0.0f, 0.0f);
    }
};
```

### 7.2 ContentUpdate — 完整选项列表

```cpp
void ContentUpdate() {
    *((int32_t*)0x6c6eb0) = 3; // 锁定游戏输入
    // → 源码: g_Supervisor.lockInputMask = 3;

    ImGui::TextUnformatted("高级选项");
    ImGui::Separator();
    ImGui::BeginChild("Adv. Options", ImVec2(0,0));

    // ① 游戏速度 (FPS)
    if (BeginOptGroup<TH_GAME_SPEED>()) {
        GameFPSOpt(mOptCtx);  // FPS设置 → FpsSet()
        EndOptGroup();
    }

    // ② Rank下降禁止
    Checkbox("禁止Rank下降", &th06_disable_drop_rank);
    // → 原版: PATCH_DY 两处 NOP 掉 rank 下降代码
    // → 源码: 在 rank 更新函数中跳过减少分支

    // ③ 按键禁用 (DisableKeyOpt)
    // → 允许禁用特定游戏按键

    // ④ 按键显示 (KeyHUDOpt)
    // → 屏幕底部渲染当前输入状态

    // ⑤ 无限命 (InfLifeOpt)
    // → 两种模式: 完全不减命 / 仅不进入Continue

    // ⑥ Boss 强制下移
    Checkbox("Boss强制下移", &forceBossMoveDown);
    DragFloat("下移范围", &g_bossMoveDownRange, 0.002, 0, 1);
    // → 原版: EHOOK 修改 boss 移动范围上界
    // → 源码: 修改 Enemy 的移动范围限制

    // ⑦ 锁定计时器
    Checkbox("自动锁定计时器", &enable_lock_timer_autoly);

    // ⑧ 显示子弹判定
    Checkbox("显示子弹判定", &g_show_bullet_hitbox);
    // → 在子弹位置绘制判定圆

    // ⑨ 显示 Rank
    Checkbox("游戏信息中显示Rank", &th06_showRank);

    // ⑩ 显示自机判定
    Checkbox("显示自机判定", &th06_show_hitbox);
    Button("重新加载判定图片");
    // → 原版: 从 hitbox.png 加载 D3D8 纹理
    // → 源码: 从文件加载 OpenGL 纹理

    // ⑪ Replay 标记
    Checkbox("显示Replay标记", &th06_showRepMarker);

    // ⑫ 固定随机种子
    Checkbox("固定随机种子", &th06_fix_seed);
    InputInt("种子值", &th06_seed); // 0-65535
    Text("上次Rep种子: %d", g_last_rep_seed);

    // ⑬ 背景修复
    Checkbox("背景渲染修复", &th06_bg_fix);
    // → 原版: PATCH_DY NOP掉3处 + EHOOK修改颜色
    // → 源码: 条件跳过背景清除代码

    // ⑭ SSS 集成
    SSS::SSS_UI(6);

    // ⑮ 符卡练习详情表格 (CollapsingHeader)
    if (CollapsingHeader("符卡练习详情")) {
        ShowDetail(nullptr); // → 见 §7.3
    }

    // ⑯ 上次六本书位置 + 复制按钮
    Text("上次: (%d,%d),...", g_books_pos[0~5]);
    Button("复制设置");

    // ⑰ 反应测试
    InGameReactionTestOpt();

    // ⑱ 关于
    AboutOpt();

    ImGui::EndChild();
}
```

### 7.3 ShowDetail — 符卡取得记录表格

```cpp
void ShowDetail(bool* isOpen) {
    ImGui::BeginTabBar("Detail Spell");
    for (int diff = 0; diff < 5; diff++) {   // Easy/Normal/Hard/Lunatic/Extra
        if (BeginTabItem(diffName[diff])) {
            BeginTabBar("Player Type");
            for (int pl = 0; pl < 4; pl++) { // ReimuA/ReimuB/MarisaA/MarisaB
                if (BeginTabItem(playerName[pl])) {

                    // 游戏时间表格 (4列)
                    BeginTable("timetable", 4);
                    // 累计总时间 | 本次时间 | 角色总时间 | 全角色总时间
                    // 显示格式: HH:MM:SS 和 YY:MM:DD HH:MM:SS 和纳秒

                    // 符卡取得表格 (5列)
                    BeginTable("sptable", 5);
                    // 符卡名 | 累计取得/挑战(%) | 累计超时 | 本次取得/挑战(%) | 本次超时
                    for (int spell = 0; spell < 65; spell++) {
                        if (is_spell_used[diff][spell]) {
                            // 渲染一行
                        }
                    }
                }
            }
        }
    }
}
```

### 7.4 FPS 控制适配

```cpp
// 原版: 检测 vpatch DLL 并修改其内存中的 FPS 值
// 源码版: 直接控制帧率目标

// 方案A: 修改 Supervisor 的帧率限制
g_Supervisor.cfg.frameskipConfig = newValue;

// 方案B: 动态修改 SDL 帧延迟目标 (更精确)
// 在 Supervisor::TickCallback() 中:
int targetFps = ThpracAdvOpt::fps; // 默认60
double frameTime = 1.0 / targetFps;
```

---

## 8. ECL 补丁系统 — 关卡跳转核心

### 8.1 ECLHelper / VFile (thprac_games.h)

thprac 通过修改 ECL 脚本字节码实现精确的关卡段落跳转。

```cpp
// VFile: 内存虚拟文件，支持位置写入
class VFile {
    void* mBuffer;
    size_t mSize, mPos;

    // 顺序写入
    template<typename T>
    VFile& operator<<(T value) {
        memcpy((char*)mBuffer + mPos, &value, sizeof(T));
        mPos += sizeof(T);
        return *this;
    }
    // 定位写入
    template<typename T>
    VFile& operator<<(pair<size_t, T> posValue) {
        memcpy((char*)mBuffer + posValue.first, &posValue.second, sizeof(T));
        return *this;
    }
    void SetPos(size_t pos) { mPos = pos; }
};

// ECLHelper: 继承 VFile，指向 ECL 数据
class ECLHelper : public VFile {
    void SetBaseAddr(void* addr) {
        // 读取 ECL 文件头的 sub 表指针
        // 设置 mBuffer 指向实际 ECL 字节码
    }
};
```

### 8.2 ECL 操作函数 (thprac_th06.cpp:1780-1830)

```cpp
// 在 ECL 中插入跳转指令
void ECLJump(ECLHelper& ecl, int32_t time, int32_t pos,
             int32_t time_jmp, int32_t target)
{
    ecl << pair{pos,   (int32_t)time}         // 设置时间
        << pair{pos+4, (int16_t)0x2}          // 指令类型=jump
        << pair{pos+6, (int16_t)0x14}         // 指令长度
        << pair{pos+8, (int32_t)0x00FFFF00}   // 标志
        << pair{pos+0xC, (int32_t)time_jmp}   // 跳转后时间
        << pair{pos+0xC+4, (int32_t)(target-pos)}; // 相对偏移
}

// 设置 ECL 全局时间 (跳过道中)
void ECLWarp(int32_t time) {
    *((int32_t*)0x5a5fb0) = time;
    // 源码: g_EclManager.currentFrameTime = time; (或等效字段)
}

// 设置 Boss 血量
void ECLSetHealth(ECLHelper& ecl, int offset, int32_t ecl_time, int32_t health) {
    ecl.SetPos(offset);
    ecl << ecl_time << 0x0010006f << 0x00ffff00 << health;
    // 0x6f = ECL opcode "set enemy health"
}

// 设置倒计时
void ECLSetTime(ECLHelper& ecl, int offset, int32_t ecl_time, int32_t time) {
    ecl.SetPos(offset);
    ecl << ecl_time << 0x00100073 << 0x00ffff00 << time;
    // 0x73 = ECL opcode "set timer"
}

// 插入等待指令 (暂停 ECL 执行)
void ECLStall(ECLHelper& ecl, int offset) {
    ecl.SetPos(offset);
    ecl << 0x7fffffff;  // time = MAX_INT → 永远不执行后续
}
```

### 8.3 源码版 ECLHelper 适配

```cpp
// 原版: ECLHelper.SetBaseAddr((void*)0x487e50)
//   → 读取 ECL 文件基址，沿指针链找到 sub 起始

// 源码版: 直接访问 g_EclManager
class ECLHelper : public VFile {
    void SetBaseAddr() {
        // g_EclManager.eclFile 指向加载的 ECL 数据
        // 等价于 *(void**)0x487e50
        void* eclBase = g_EclManager.eclFile;

        // 读取 ECL 头部的 sub 表
        // eclBase + header.subOffset → sub[0] 起始地址
        // 设置 mBuffer = subStart
        // 设置 mSize = subSize
    }
};
```

### 8.4 THSectionPatch (thprac_th06.cpp:2804-2830)

ECL 补丁的分发入口:

```cpp
void THSectionPatch() {
    ECLHelper ecl;
    ecl.SetBaseAddr((void*)0x487e50);
    // → 源码: ecl.SetBaseAddr(g_EclManager.eclFile);

    auto section = thPracParam.section;

    if (section >= 10000 && section < 20000) {
        // 章节跳转 (Chapter warp)
        int stage = (section / 100) % 100;
        int chapter = section % 100;
        THStageWarp(ecl, stage, chapter);
    } else if (section) {
        // 具体段落跳转 (Boss/符卡)
        THPatch(ecl, section);
    }
}
```

### 8.5 THPatch 关键跳转示例 (thprac_th06.cpp:1835-2803)

约 1000 行，对每个 `th_sections_t` 值编写精确的 ECL 字节码修改。

```cpp
void THPatch(ECLHelper& ecl, int section) {
    switch (section) {
    // --- Stage 1 ---
    case TH06_ST1_MID1: // 道中 Boss (大妖精)
        ECLJump(ecl, 0, 0x6a0, 0, 0x754);
        // 跳转到大妖精出现的 ECL 位置
        ecl << pair{0x778, (int16_t)0x80};
        break;

    case TH06_ST1_BOSS1: // 露米娅非符1
        if (thPracParam.dlg)
            ECLJump(ecl, 0, 0x6a0, 1800, 0xf24);
        else
            ECLJump(ecl, 0, 0x6a0, 1860, 0x107c);
        break;

    case TH06_ST1_BOSS2: // 月符「月光」
        ECLJump(ecl, 0, 0x6a0, 1860, 0x107c);
        ECLStall(ecl, 0x11a4);
        // 跳过非符1，直接到符卡
        break;

    // ... 每个 section 对应精确的 ECL 偏移量 ...

    // --- Stage 4 Books ---
    case TH06_ST4_BOOKS:
        // 设置6本书的自定义位置
        for (int i = 0; i < 6; i++) {
            if (thPracParam.bF & (1 << i)) {
                // 写入书的 X/Y 位置到 ECL 数据
            }
        }
        break;

    // --- Stage 6 Boss9 (墙练) ---
    case TH06_ST6_BOSS9:
        ECLJump(ecl, 0, ...); // 跳到最终符
        if (thPracParam.phase) {
            // 设置特定阶段
        }
        if (thPracParam.delay_st6bs9) {
            // 设置延迟帧
        }
        break;

    // ... 共约 60+ case 分支 ...
    }
}
```

> **适配要点**: ECL 偏移量是与 ECL 脚本数据结构绑定的，不依赖 exe 地址。
> 只需确保 `ecl.SetBaseAddr()` 正确指向 `g_EclManager.eclFile` 的 sub 数据区域，
> 所有 ECLJump/ECLStall/ECLSetHealth 等操作在源码版中完全相同。

---

## 9. 游戏信息叠加层 (TH06InGameInfo) & 符卡记录 (TH06Save)

### 9.1 TH06InGameInfo (thprac_th06.cpp:1300-1425)

```cpp
class TH06InGameInfo : public Gui::GameGuiWnd {
    TH06InGameInfo() noexcept {
        SetFade(0.9f, 0.9f);
        SetSizeRel(180.0f / 640.0f, 0.0f);
        SetPosRel(433.0f / 640.0f, 245.0f / 480.0f);
        SetWndFlag(NoScrollbar | NoTitleBar | NoResize | NoMove |
                   NoSavedSettings | NoInputs | NoFocusOnAppearing | NoNav);
    }
    // 计数器
    int32_t mMissCount;
    struct BooksInfo {
        bool is_books;
        int32_t time_books;
        bool is_died;
        int32_t miss_count;
        int32_t bomb_count;
    } booksInfo;

    void GameStartInit() {
        mMissCount = 0;
        booksInfo = {};
    }
};
```

显示内容 (由 OnContentUpdate 渲染):
- 难度 + 弹型 名称
- 分数 / 残机 / Bomb / 火力 / 擦弹 / 点数
- Rank (如果 showRank 开启)
- FPS
- 当前Miss次数
- 六本书练习时: 存活时间 + miss/bomb次数

### 9.2 TH06Save — 符卡取得记录系统 (thprac_th06.cpp:48-230)

```cpp
class TH06Save {
    struct Save {
        int32_t SC_History[65][5][4][3]; // [spell_id][diff][player_type][capture/attempt/timeout]
        int64_t timePlayer[5][4];        // [diff][player_type] 精度=纳秒
    };
    Save save_current; // 本次游戏
    Save save_total;   // 累计

    void LoadSave() {
        // 从 %appdata%/ShanghaiAlice/th06/score06.dat 读取
        // 兼容旧版 spell_capture.dat
    }
    void SaveSave() {
        // 写入 score06.dat (version=1, 然后 save_total)
    }

    void AddAttempt(int spell_id, byte diff, byte player_type) {
        save_total.SC_History[spell_id][diff][player_type][1]++;
        save_current.SC_History[spell_id][diff][player_type][1]++;
        SaveSave();
    }
    void AddCapture(spell_id, diff, player_type) { /* 类似 */ }
    void AddTimeOut(spell_id, diff, player_type) { /* 类似 */ }

    void IncreaseGameTime() {
        // 每帧调用 (非Replay、游戏中、非暂停时)
        // 使用高精度时钟测量帧间隔
        // 累加到 timePlayer[diff][player_type]
        // 每3分钟自动保存
    }
};
```

### 9.3 源码版适配

```cpp
// 数据文件路径: 保持与 thprac 兼容
// %appdata%/ShanghaiAlice/th06/score06.dat

// 符卡ID映射: 需要在 EHOOK 触发点识别当前符卡ID
// 原版 EHOOK 钩住符卡开始/结束/超时的代码点
// 源码版: 在 EclManager 或 Enemy 的符卡相关函数中插入回调

// 时间记录: 使用 SDL_GetPerformanceCounter() 替代 Win32 性能计数器
```

---

## 10. EHOOK 映射表 — 原版钩子→源码插入点

### 10.1 主循环钩子

| EHOOK名 | 原版地址 | 源码插入位置 | 功能 |
|---------|---------|-------------|------|
| `th06_update` | `0x41caac` | `Supervisor::TickCallback()` 每帧末尾 | ImGui 更新所有 GUI 组件 |
| `th06_render` | `0x41cb6d` | `Supervisor::DrawCallback()` 渲染后 | `GameGuiRender()` |

### 10.2 菜单/选关钩子

| EHOOK名 | 原版地址 | 源码插入位置 | 功能 |
|---------|---------|-------------|------|
| `th06_prac_menu_1` | `0x437179` | `MainMenu` 练习选择后 | `THGuiPrac::State(1)` — 打开练习菜单 |
| `th06_prac_menu_3` | `0x43738c` | `MainMenu` 确认开始后 | `THGuiPrac::State(3)` — 关闭并填充参数 |
| `th06_prac_menu_4` | `0x43723f` | `MainMenu` 取消时 | `THGuiPrac::State(4)` — 关闭不填充 |
| `th06_prac_menu_enter` | `0x4373a3` | `MainMenu` 进入关卡时 | 设置 currentStage/nextStage/difficulty |
| `th06_rep_menu_1` | `0x438262` | `ReplayMenu` 打开时 | `THGuiRep::State(1)` — 重置状态 |
| `th06_rep_menu_2` | `0x4385d5` | `ReplayMenu` 选择Replay后 | `THGuiRep::State(2)` — 检查Replay参数 |
| `th06_rep_menu_3` | `0x438974` | `ReplayMenu` 确认播放 | `THGuiRep::State(3)` — 加载参数 |

### 10.3 游戏逻辑钩子

| EHOOK名 | 原版地址 | 源码插入位置 | 功能 |
|---------|---------|-------------|------|
| `th06_patch_main` | `0x41c17a` | `GameManager::AddedCallback()` | 应用练习参数到游戏状态 + ECL补丁 |
| `th06_pause_menu` | `0x401b8f` | `Supervisor` 暂停处理 | 接管暂停菜单逻辑 |
| `th06_restart` | `0x435901` | 重启流程入口 | 判断重启来源(练习/正常) |
| `th06_title` | `0x41ae2c` | 标题画面跳过 | 练习模式跳过标题动画 |
| `th06_inf_lives` | `0x428DEB` | `Player` 死亡减命后 | 无限命补偿 (+1) |
| `th06_miss` | `0x428DD9` | `Player` 死亡时 | 增加 mMissCount |
| `th06_cancel_muteki` | `0x429ec4` | 练习模式被弹后 | 清除无敌帧(让练习更真实) |
| `th06_set_deathbomb_timer` | `0x42a09c` | 练习模式被弹后 | 设 deathbomb 窗口=6帧 |
| `th06_fake_shot_type` | `0x40b2f9` | ECL 读取弹型时 | 伪装为指定弹型 |
| `th06_patchouli` | `0x40c100` | 帕秋莉属性初始化 | 根据 fakeType 修改属性 |
| `th06_save_replay` | `0x42b03b` | 保存Replay时 | 嵌入 thPracParam JSON |
| `th06_fix_seed` | `0x41BE47` | 随机种子初始化时 | 覆写为固定种子 |

### 10.4 BGM 相关钩子

| EHOOK名 | 原版地址 | 源码插入位置 | 功能 |
|---------|---------|-------------|------|
| `th06_bgm_play` | `0x430f20` | `MidiOutput::Play()` | 记录BGM切换 |
| `th06_bgm_stop` | `0x430f80` | `MidiOutput::Stop()` | 永续BGM: 跳过停止 |
| `th06_pause_menu_pauseBGM` | `0x402714` | 暂停/恢复时 | 暂停时停止/恢复BGM |

### 10.5 墙练 / 特殊钩子

| EHOOK名 | 原版地址 | 源码插入位置 | 功能 |
|---------|---------|-------------|------|
| `th06_wall_prac` | `0x40D57C` | 墙弹角度计算处 | 修改墙弹发射角度(狙击) |
| `th06_wall_prac2` | `0x40D900` | 子弹角度计算处 | 根据 snipeN/F 调整狙击率 |
| `th06_wall_prac_boss_pos` | `0x40907F` | Boss位置更新后 | 记录boss坐标供墙练用 |
| `th06_hamon_rage` | `0x40e1c7` | Extra最终符特殊处理 | 阶段1跳过特定效果 |
| `th06_lock_timer1~4` | 多处 | 各计时器操作点 | 锁定计时器功能 |

### 10.6 主循环详细代码 (th06_update)

```cpp
EHOOK_DY(th06_update, 0x41caac, 1, {
    // 1. 初始化 ImGui 帧
    GameGuiBegin(IMPL_WIN32_DX8, !THAdvOptWnd::singleton().IsOpen());
    // → 源码: ImGui_ImplOpenGL2_NewFrame() 等已在别处调用
    //   此处只需: 如果F12窗口打开则锁定游戏输入

    // 2. 更新输入状态
    Gui::KeyboardInputUpdate(VK_ESCAPE);
    // → 源码: InGameInputGet() 已处理

    // 3. 更新所有 GUI 组件 (状态机驱动)
    THPauseMenu::singleton().Update();
    THGuiPrac::singleton().Update();
    THGuiRep::singleton().Update();
    THOverlay::singleton().Update();
    TH06InGameInfo::singleton().Update();

    // 4. 六本书计时
    if (TH06InGameInfo::singleton().booksInfo.is_books)
        TH06InGameInfo::singleton().booksInfo.time_books++;

    // 5. 游戏时间记录
    TH06Save::singleton().IncreaseGameTime();

    // 6. 叠加层渲染 (DrawList)
    auto p = ImGui::GetOverlayDrawList();
    RenderPlHitbox(ImGui::GetBackgroundDrawList()); // 自机判定
    RenderRepMarker(p);                              // Replay标记
    RenderBtHitbox(p);                               // 子弹判定
    RenderLockTimer(p);                              // 锁定计时器

    // 7. 按键显示
    if (show_keyboard_monitor && gameState == 2) {
        KeysHUD(6, {1280,0}, {833,0}, keyboard_style);
    }

    // 8. Boss强制下移提示
    if (forceBossMoveDown) {
        p->AddRectFilled(...); p->AddText(..., "Boss强制下移");
    }

    // 9. 结束帧
    GameGuiEnd(advOptOpen || pracOpen || pauseOpen);
});
```

---

## 11. 关卡参数应用流程 (th06_patch_main)

这是练习模式的核心: 在关卡加载后将 thPracParam 应用到游戏状态。

### 11.1 原版 EHOOK (thprac_th06.cpp:3195-3240)

```cpp
EHOOK_DY(th06_patch_main, 0x41c17a, 5, {
    // 源码位置: GameManager::AddedCallback() 开头
    // 关卡数据已加载完毕，ECL 已在内存中

    THPauseMenu::singleton().el_bgm_changed = false;

    if (thPracParam.mode == 1) {
        // 应用基础参数
        *(int8_t*)(0x69d4ba) = (int8_t)thPracParam.life;
        // → g_GameManager.livesRemaining = thPracParam.life;

        *(int8_t*)(0x69d4bb) = (int8_t)thPracParam.bomb;
        // → g_GameManager.bombsRemaining = thPracParam.bomb;

        *(int16_t*)(0x69d4b0) = (int16_t)thPracParam.power;
        // → g_GameManager.currentPower = thPracParam.power;

        *(int32_t*)(0x69bca0) = *(int32_t*)(0x69bca4) = (int32_t)thPracParam.score;
        // → g_GameManager.guiScore = g_GameManager.score = thPracParam.score;

        *(int32_t*)(0x69bcb4) = *(int32_t*)(0x69bcb8) = (int32_t)thPracParam.graze;
        // → g_GameManager.grazeInStage = g_GameManager.grazeTotal = thPracParam.graze;

        *(int16_t*)(0x69d4b4) = *(int16_t*)(0x69d4b6) = (int16_t)thPracParam.point;
        // → g_GameManager.pointItemsCollectedInStage =
        //   g_GameManager.pointItemsCollected = thPracParam.point;

        *(uint32_t*)0x5a5fb0 = thPracParam.frame;
        // → g_EclManager.currentFrameTime = thPracParam.frame;

        // Extend(续关)进度根据分数设置
        if (difficulty != 4) { // 非Extra
            if (score >= 60000000) extraLives = 4;
            else if (score >= 40000000) extraLives = 3;
            else if (score >= 20000000) extraLives = 2;
            else if (score >= 10000000) extraLives = 1;
        }

        // Rank
        *(int32_t*)(0x69d710) = thPracParam.rank;
        // → g_GameManager.rank = thPracParam.rank;
        if (thPracParam.rankLock) {
            *(int32_t*)(0x69d714) = thPracParam.rank; // maxRank
            *(int32_t*)(0x69d718) = thPracParam.rank; // minRank
        }

        // ECL 补丁 (关卡跳转)
        THSectionPatch();
    }

    thPracParam._playLock = true;

    // BGM 处理
    if (THPauseMenu::singleton().el_bgm_signal) {
        // 永续BGM: 跳过BGM重置
        THPauseMenu::singleton().el_bgm_signal = false;
        pCtx->Eip = 0x41c18a; // 跳过 BGM 初始化
    } else if (THBGMTest()) {
        // 有自定义BGM: 调整 BGM 初始化
        pCtx->Eax += 0x310;
        pCtx->Eip = 0x41c17f;
    }
});
```

### 11.2 源码版实现

```cpp
// 在 GameManager::AddedCallback() 中，关卡数据加载完成后:
void GameManager::AddedCallback() {
    // ... 原有的关卡加载逻辑 ...

    // thprac 参数应用 (新增)
    if (g_thPracParam.mode == 1) {
        g_GameManager.livesRemaining = (i8)g_thPracParam.life;
        g_GameManager.bombsRemaining = (i8)g_thPracParam.bomb;
        g_GameManager.currentPower = (u16)g_thPracParam.power;
        g_GameManager.guiScore = g_GameManager.score = (u32)g_thPracParam.score;
        g_GameManager.grazeInStage = g_GameManager.grazeTotal = g_thPracParam.graze;
        g_GameManager.pointItemsCollectedInStage =
            g_GameManager.pointItemsCollected = (u16)g_thPracParam.point;

        // Extend 进度
        if (g_GameManager.difficulty != 4) {
            if (g_thPracParam.score >= 60000000) g_GameManager.extraLives = 4;
            else if (g_thPracParam.score >= 40000000) g_GameManager.extraLives = 3;
            else if (g_thPracParam.score >= 20000000) g_GameManager.extraLives = 2;
            else if (g_thPracParam.score >= 10000000) g_GameManager.extraLives = 1;
        }

        g_GameManager.rank = g_thPracParam.rank;
        if (g_thPracParam.rankLock) {
            g_GameManager.maxRank = g_thPracParam.rank;
            g_GameManager.minRank = g_thPracParam.rank;
        }

        // ECL 补丁
        ThpracSectionPatch();
    }
    g_thPracParam.playLock = true;
}
```

---

## 12. 文件结构与实施计划

### 12.1 新增/修改文件清单

```
src/
├── ThpracParam.hpp        [新增] THPracParam 结构体 + 全局实例
├── ThpracSections.hpp     [新增] th_sections_t 枚举 + 查找表
├── ThpracSections.cpp     [新增] 查找表数据 (th_sections_cba/cbt/str/bgm)
├── ThpracEcl.hpp          [新增] ECLHelper/VFile + ECLJump/Warp/等
├── ThpracEcl.cpp          [新增] THPatch() + THSectionPatch() + THStageWarp()
├── ThpracOverlay.hpp      [新增] BKSP叠加菜单 + F1-F8作弊
├── ThpracOverlay.cpp      [新增] DrawOverlay() + ApplyCheats()
├── ThpracPracMenu.hpp     [新增] 练习菜单 + 暂停菜单
├── ThpracPracMenu.cpp     [新增] PracticeMenu() + PauseMenu状态机
├── ThpracAdvOpt.hpp       [新增] F12高级选项
├── ThpracAdvOpt.cpp       [新增] ContentUpdate() 完整选项
├── ThpracSave.hpp         [新增] TH06Save 符卡记录
├── ThpracSave.cpp         [新增] LoadSave/SaveSave/AddCapture/等
├── ThpracGui.hpp          [重写] 统一入口: Init/Update/Render
├── ThpracGui.cpp          [重写] 组织所有子模块的更新/渲染
│
├── GameManager.cpp        [修改] AddedCallback() 中插入参数应用
├── Supervisor.cpp         [修改] TickCallback() 中插入暂停接管
├── MainMenu.cpp           [修改] 练习选择后调用 State(1/3/4)
├── Player.cpp             [修改] 自动Bomb/被弹处理修改
├── EclManager.hpp         [修改] 暴露 eclFile 指针供 ECLHelper 使用
└── Enemy.cpp              [修改] Boss强制下移/符卡开始结束回调
```

### 12.2 实施顺序 (建议分阶段)

**阶段 1: 基础框架**
1. `ThpracParam.hpp/cpp` — 参数结构体
2. `ThpracSections.hpp/cpp` — 枚举和查找表数据
3. 重写 `ThpracGui.hpp/cpp` — 统一入口

**阶段 2: UI 系统**
4. `ThpracOverlay.cpp` — BKSP 叠加 + F1-F8 作弊
5. `ThpracPracMenu.cpp` — 练习菜单完整 UI (含 Section/Phase/Books 选择器)
6. `ThpracAdvOpt.cpp` — F12 高级选项完整 UI

**阶段 3: 游戏逻辑集成**
7. `ThpracEcl.cpp` — ECLHelper + THPatch (核心: ~1000行 ECL 字节码)
8. 修改 `GameManager.cpp` — 参数应用入口
9. 修改 `MainMenu.cpp` — 练习菜单状态机触发
10. 修改 `Supervisor.cpp` — 暂停菜单接管

**阶段 4: 高级功能**
11. `ThpracSave.cpp` — 符卡记录系统
12. 暂停菜单完整版 (含练习设置、快速重开)
13. 六本书位置编辑器
14. 墙练系统 (wall_prac)
15. 自机/子弹判定显示
16. 固定随机种子
17. Boss 强制下移

### 12.3 当前实现差距分析

现有 `ThpracGui.cpp` (已提交) vs 完整 thprac:

| 功能 | 现有状态 | 完整版需求 |
|------|---------|-----------|
| BKSP 叠加菜单 | ✅ 基本完成 | ≈完成 |
| F1-F8 作弊 | ✅ F1-F5 有效, F6/F7 待实现 | F6=自动Bomb需Player钩, F7=永续BGM需Midi钩 |
| 游戏信息叠加 | ✅ 基本完成 | 缺 Miss计数, Books信息 |
| 按键显示 | ✅ 基本完成 | ≈完成 |
| 练习菜单 | ⚠️ 仅 Mode/Stage/基础参数 | 缺 Warp/Section/Phase/Books/FakeShot/WallPrac |
| F12 高级选项 | ⚠️ 仅 FPS/Rank显示/作弊镜像 | 缺 18项中的13项 |
| 暂停菜单接管 | ❌ 未实现 | 完整状态机 + 练习设置入口 |
| ECL 补丁 | ❌ 未实现 | ~1000行 ECLHelper + THPatch |
| 符卡记录 | ❌ 未实现 | TH06Save + ShowDetail 表格 |
| Replay 参数 | ❌ 未实现 | THGuiRep + JSON 嵌入 |
| 六本书编辑 | ❌ 未实现 | 位置编辑器 + 镜像/随机/复制 |
| 墙练 | ❌ 未实现 | 角度修改 + 狙击率控制 |
| 判定显示 | ❌ 未实现 | 自机/子弹判定圆 |
| 随机种子固定 | ❌ 未实现 | 种子覆写 |
| Boss 强制下移 | ❌ 未实现 | Enemy 移动范围修改 |
| 快速重开(R键) | ❌ 未实现 | 非练习模式也可按R重开 |

---

*文档结束。基于此文档可逐阶段实施完整的 thprac 练习工具移植。*
