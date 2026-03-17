#pragma once
// thprac_games.h - Adapter for th06 source build
// Provides the main game framework interface that thprac_th06.cpp depends on

// __stdcall is a Windows calling convention; no-op on other platforms
#if !defined(_MSC_VER) && !defined(__stdcall)
#define __stdcall
#endif

#include "thprac_gui_components.h"
#include "thprac_hook.h"
#include "thprac_utils.h"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

#define BOSS_MOVE_DOWN_RANGE_INIT 0.5

// Forward-declare SDL_Window in global namespace so THPrac::SetGuiWindow works
struct SDL_Window;

namespace THPrac {

// Forward declarations
void GameUpdateInner(int gamever);
void GameUpdateOuter(ImDrawList* p, int ver);
void FastRetry(int thprac_mode);

ImTextureID ReadImage(DWORD dxVer, DWORD device, const char* name, const char* srcData, size_t srcSz);

// ============================================================
// Key Render (from thprac_igi_key_render.h)
// ============================================================
struct KeyRectStyle {
    bool separated = true;
    bool show_aps = true;
    uint32_t fill_color_press = 0xFFFFFFFF;
    uint32_t fill_color_release = 0xFFFFFFFF;
    uint32_t border_color_press = 0xFFFF4444;
    uint32_t border_color_release = 0xFFFFCCCC;
    uint32_t text_color_press = 0xFFFFFFFF;
    uint32_t text_color_release = 0xFFFFFFFF;
    ImVec2 padding = { 0.05f, 0.05f };
    ImVec2 size = { 34.0f, 34.0f };
    int type = 2;
};
void RecordKey(int ver, uint32_t cur_key);
void KeysHUD(int ver, ImVec2 render_pos_arrow, ImVec2 render_pos_key, const KeyRectStyle& style, bool align_right_arrow = true, bool align_right_key = false);
void SaveKeyRecorded();
void ClearKeyRecord();
std::vector<int>& GetKeyAPS();

// ============================================================
// Advanced Game Options
// ============================================================
struct AdvancedGameOptions {
    bool enable_lock_timer_autoly = false;
    bool map_inf_life_to_no_continue = true;
    bool th06_bg_fix = false;
    bool th06_fix_seed = false;
    int32_t th06_seed = 0;
    bool th06_pauseBGM = false;
    bool th06_disable_drop_rank = false;
    bool th06_showRank = false;
    bool th06_show_hitbox = false;
    bool th06_showRepMarker = false;
    bool th06_autoname = false;
    char th06_autoname_name[12] = {0};
    bool show_keyboard_monitor = false;
    KeyRectStyle keyboard_style;
};
extern AdvancedGameOptions g_adv_igi_options;

// ============================================================
// Structs
// ============================================================
struct Float2 {
    float x;
    float y;
};

struct Float3 {
    float x;
    float y;
    float z;
};

struct Timer {
    int32_t previous;
    int32_t current;
    float current_f;
    float* __game_speed__disused;
    uint32_t control;
};

// ============================================================
// Game GUI
// ============================================================
enum game_gui_impl {
    IMPL_WIN32_DX8,
    IMPL_WIN32_DX9
};

void SetDpadHook(uintptr_t addr, size_t instr_len);
void SetGuiWindow(SDL_Window* w);
void GameGuiInit(game_gui_impl impl, int device, int hwnd_addr,
    Gui::ingame_input_gen_t input_gen, int reg1, int reg2, int reg3 = 0,
    int wnd_size_flag = -1, float x = 640.0f, float y = 480.0f);
extern int GameGuiProgress;
extern int GameGuiGeneration;
extern bool g_forceRenderCursor;
void GameGuiBegin(game_gui_impl impl, bool game_nav = true);
void GameGuiEnd(bool draw_cursor = false);
void GameGuiRender(game_gui_impl impl);

// ============================================================
// Advanced Options Menu
// ============================================================
struct adv_opt_ctx {
    int fps_status = 0;
    int fps = 60;
    double fps_dbl = 1.0 / 60.0;
    int fps_replay_slow = 0;
    int fps_replay_fast = 0;
    int fps_debug_acc = 0;
    uintptr_t vpatch_base = 0;

    std::wstring data_rec_dir;

    bool all_clear_bonus = false;

    typedef bool __stdcall oilp_set_fps_t(int fps);
    oilp_set_fps_t* oilp_set_game_fps = NULL;
    oilp_set_fps_t* oilp_set_replay_skip_fps = NULL;
    oilp_set_fps_t* oilp_set_replay_slow_fps = NULL;
};

void OILPInit(adv_opt_ctx& ctx);
void CenteredText(const char* text, float wndX);
float GetRelWidth(float rel);
float GetRelHeight(float rel);
void CalcFileHash(const wchar_t* file_name, uint64_t hash[2]);
void HelpMarker(const char* desc);
void CustomMarker(const char* text, const char* desc);

template <th_glossary_t name>
static bool BeginOptGroup()
{
    static bool group_status = true;
    ImGui::SetNextItemOpen(group_status);
    group_status = ImGui::CollapsingHeader(Gui::LocaleGetStr(name), ImGuiTreeNodeFlags_None);
    if (group_status)
        ImGui::Indent();
    return group_status;
}
inline void EndOptGroup()
{
    ImGui::Unindent();
}

typedef void __stdcall FPSHelperCallback(int32_t);
int FPSHelper(adv_opt_ctx& ctx, bool repStatus, bool vpFast, bool vpSlow, FPSHelperCallback* callback);
bool GameFPSOpt(adv_opt_ctx& ctx, bool replay = true);
bool GameplayOpt(adv_opt_ctx& ctx);
void AboutOpt(const char* thanks_text = nullptr);
void InGameReactionTestOpt();
void InfLifeOpt();
void DisableKeyOpt();
void KeyHUDOpt();

// ============================================================
// Replay System
// ============================================================
bool ReplaySaveParam(const wchar_t* rep_path, const std::string& param);
bool ReplayLoadParam(const wchar_t* rep_path, std::string& param);

// ============================================================
// String Encoding
// ============================================================
std::wstring mb_to_utf16(const char* str, unsigned int encoding);

// ============================================================
// SSS (Section/Stage Select)
// ============================================================
namespace SSS {
    void SSS_UI(int version);
    void SSS_Update(int ver);
}

// ============================================================
// JSON Macros (using rapidjson)
// ============================================================
#define ParseJson()                                \
    Reset();                                       \
    rapidjson::Document param;                     \
    if (param.Parse(json.c_str()).HasParseError()) \
        return false;
#define CreateJson()           \
    rapidjson::Document param; \
    param.SetObject();         \
    auto& jalloc = param.GetAllocator();
#define ReturnJson()                                       \
    rapidjson::StringBuffer sb;                            \
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb); \
    param.Accept(writer);                                  \
    return sb.GetString();
#define ForceJsonValue(value_name, comparator)                             \
    if (!param.HasMember(#value_name) || param[#value_name] != comparator) \
        return false;
#define GetJsonValue(value_name)                                       \
    if (param.HasMember(#value_name) && param[#value_name].IsNumber()) \
        value_name = (decltype(value_name))param[#value_name].GetDouble();
#define GetJsonValueEx(value_name, type)                               \
    if (param.HasMember(#value_name) && param[#value_name].Is##type()) \
        value_name = (decltype(value_name))param[#value_name].Get##type();
#define AddJsonValue(value_name)                                           \
    {                                                                      \
        rapidjson::Value __key_##value_name(#value_name, jalloc);          \
        rapidjson::Value __value_##value_name(value_name);                 \
        param.AddMember(__key_##value_name, __value_##value_name, jalloc); \
    }
#define AddJsonValueEx(value_name, ...)                                    \
    {                                                                      \
        rapidjson::Value __key_##value_name(#value_name, jalloc);          \
        rapidjson::Value __value_##value_name(__VA_ARGS__);                \
        param.AddMember(__key_##value_name, __value_##value_name, jalloc); \
    }
#define GetJsonArray(value_name, value_len)                                                        \
    {                                                                                              \
        if (param.HasMember(#value_name) && param[#value_name].IsArray())                          \
            for (size_t i = 0; i < std::min(param[#value_name].Size(), value_len); i++)            \
                if (param[#value_name][i].IsNumber())                                              \
                    value_name[i] = (decltype(+value_name[i]))(param[#value_name][i].GetDouble()); \
    }
#define AddJsonArray(value_name, value_len)                                \
    {                                                                      \
        rapidjson::Value __key_##value_name(#value_name, jalloc);          \
        rapidjson::Value __value_##value_name(rapidjson::kArrayType);      \
        __value_##value_name.SetArray();                                   \
        for (int i = 0; i < value_len; i++)                                \
            __value_##value_name.PushBack(value_name[i], jalloc);          \
        param.AddMember(__key_##value_name, __value_##value_name, jalloc); \
    }

// ============================================================
// Virtual File System (stubs for source build)
// ============================================================
enum VFS_TYPE {
    VFS_TH11,
    VFS_TH08,
    VFS_TH07,
    VFS_TH06,
};

} // namespace THPrac

// Forward declare FastRetryOpt (defined in thprac_th06.cpp)
namespace THPrac {
    struct FastRetryOpt {
        static constexpr int fast_retry_cout_down_max = 15;
        bool enable_fast_retry = false;
        int fast_retry_count_down = 0;
    };
    extern FastRetryOpt g_fast_re_opt;
}
