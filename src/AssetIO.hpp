#pragma once
// =============================================================================
// AssetIO.hpp — Unified read-only asset opener (cross-platform)
//
// Centralises the platform-specific I/O quirks that previously lived in three
// separate places (FileSystem.cpp, VkResources::LoadSpvFile, zwave OGG probe).
// Every read-only asset path (shaders, .dat archives, raw assets, OGG/WAV
// siblings, MIDI files, .anm/.png) flows through here so callers can stay
// platform-agnostic.
//
// Search-root order on desktop (first hit wins):
//   1. cwd-relative   (also handles absolute paths transparently)
//   2. exe-dir-relative (resolved via SDL_GetBasePath at Init time)
//
// On Android: SDL_RWFromFile is used directly — SDL2 routes relative paths
// to the APK AAssetManager and absolute paths to the filesystem.
//
// Init() additionally chdir()s to the exe directory on desktop so that legacy
// callers that still hand-craft fopen() paths (logs, ConfigStore, replay
// writes, …) keep working from any launch directory (Explorer, IDE F5, etc.).
// =============================================================================

#include <SDL.h>
#include <stddef.h>

namespace th06
{
namespace AssetIO
{

// Call once at program start, before any asset I/O.
void Init(int argc, char **argv);

// Returns the executable's directory with a trailing separator (e.g.
// "C:\\path\\to\\Release\\"), or "" if it could not be determined / on Android.
const char *BaseDir();

// Open a read-only asset. Returns NULL if not found in any search root.
// The returned SDL_RWops owns the underlying handle and must be closed by
// the caller via SDL_RWclose.
SDL_RWops *OpenRW(const char *relPath);

// Read an entire asset into a malloc()'d buffer. Caller must free().
// On failure, returns NULL and sets *outSize=0 (if outSize != NULL).
unsigned char *ReadAll(const char *relPath, size_t *outSize);

// Append a line to diag_log.txt (in cwd, line-buffered, always-on).
// Used by AssetIO + zwave + Supervisor for filesystem/OGG/restart tracing.
// Format: "[HH:MM:SS.mmm tag] message\n"
void DiagLog(const char *tag, const char *fmt, ...);

} // namespace AssetIO
} // namespace th06
