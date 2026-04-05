#pragma once

#include "NetplayInternal.hpp"

namespace th06::Netplay::AuthoritativeReplicator
{
struct ReplicatedWorldState : public AuthoritativeFrameState
{
};

void Reset();
bool CaptureCurrentReplicatedWorldState(int serverFrame, ReplicatedWorldState &outState);
void ApplyLatestAuthoritativeStateToLiveWorld();
void RecordLocalMirrorFrameHash(int frame);
bool TryConsumeHostMismatchFrame(int &outMismatchFrame);
} // namespace th06::Netplay::AuthoritativeReplicator
