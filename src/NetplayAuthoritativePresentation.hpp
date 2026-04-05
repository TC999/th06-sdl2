#pragma once

#include "NetplayInternal.hpp"

namespace th06::Netplay::AuthoritativePresentation
{
struct LocalPredictionState
{
    bool valid = false;
    bool localIsPlayer1 = true;
    int visualFrame = -1;
    u16 inputBits = 0;
    D3DXVECTOR3 displayPosition {};
    D3DXVECTOR3 authoritativePosition {};
    D3DXVECTOR3 authoritativeOrbs[2] {};
};

void Reset();
void NoteLocalPredictedInput(int frame, u16 inputBits);
void SyncFromCanonicalLocalPlayer(int frame);
void ReconcileFromAuthoritativeState(const AuthoritativeFrameState &state);
bool TryGetRenderOverride(const Player *player, D3DXVECTOR3 &outPosition, D3DXVECTOR3 outOrbs[2]);
} // namespace th06::Netplay::AuthoritativePresentation
