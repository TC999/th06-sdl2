#pragma once
// thprac_gui_integration.h - Glue between th06 game loop and thprac GUI
// Provides ImGui SDL2+OpenGL2 initialization and per-frame hooks.

struct SDL_Window;
union SDL_Event;

namespace THPrac {

// Call once after SDL window + GL context are created.
void THPracGuiInit(SDL_Window* window, void* glContext);

// Call for every SDL event in the event loop.
void THPracGuiProcessEvent(SDL_Event* event);

// Call once per frame after RunCalcChain (builds ImGui draw data).
void THPracGuiUpdate();

// Call in EndFrame after FBO blit, before SDL_GL_SwapWindow (submits draw data to GPU).
void THPracGuiRender();

// Returns true after THPracGuiInit has completed.
bool THPracGuiIsReady();

// CE-style game speed multiplier. 1.0 = normal, 2.0 = 2x, 0.5 = half speed.
// Affects frame timing only (not game logic), so replays stay in sync.
float THPracGetSpeedMultiplier();

} // namespace THPrac
