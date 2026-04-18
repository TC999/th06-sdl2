#pragma once
// thprac_gui_integration.h - Glue between th06 game loop and thprac GUI
// Provides ImGui SDL2+OpenGL2 initialization and per-frame hooks.

struct SDL_Window;
union SDL_Event;

namespace THPrac {

// Call once after SDL window + GL context are created.
void THPracGuiInit(SDL_Window* window, void* glContext);

// Vulkan path (Phase 5b.1): init ImGui context + font + thprac singletons
// WITHOUT any rendering backend (no GL, no Vulkan ImGui backend yet). This
// keeps ImGui::GetIO() and other introspection APIs functional so thprac
// singletons (THOverlay etc.) can be safely constructed via inline accessors
// (THPracIsTimeLock etc.) called from the game's per-frame chain. The UI is
// not rendered until imgui_impl_vulkan is vendored (Phase 5b.2).
void THPracGuiInitHeadless(SDL_Window* window);

// Call for every SDL event in the event loop.
void THPracGuiProcessEvent(SDL_Event* event);

// Call once per frame after RunCalcChain (builds ImGui draw data).
void THPracGuiUpdate();

// Call in EndFrame after FBO blit, before SDL_GL_SwapWindow (submits draw data to GPU).
void THPracGuiRender();

// Shut down ImGui backends and destroy context (call before GL context destruction).
void THPracGuiShutdown();

// Returns true after THPracGuiInit has completed.
bool THPracGuiIsReady();

// CE-style game speed multiplier. 1.0 = normal, 2.0 = 2x, 0.5 = half speed.
// Affects frame timing only (not game logic), so replays stay in sync.
float THPracGetSpeedMultiplier();

} // namespace THPrac
