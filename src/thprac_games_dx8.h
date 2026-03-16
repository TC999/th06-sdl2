#pragma once
// thprac_games_dx8.h - Stub for th06 source build
// Provides THSnapshot namespace stubs (screenshots not implemented in SDL2 build)

#include <cstdint>

struct IDirect3DDevice8;

namespace THPrac {
namespace THSnapshot {
    inline void* GetSnapshotData(IDirect3DDevice8* /*d3d8*/) { return nullptr; }
    inline void Snapshot(IDirect3DDevice8* /*d3d8*/) {}
}
}
