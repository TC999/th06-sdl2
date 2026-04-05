#pragma once

#include "NetplayAuthoritativeReplicator.hpp"

namespace th06::Netplay
{
bool SendAuthoritativeFrameStateDatagram(const AuthoritativeReplicator::ReplicatedWorldState &state);
bool HandleAuthoritativeStateDatagram(const AuthoritativeStateDatagramHeader &header);
bool HandleAuthoritativeStateDatagramBytes(const void *bytes, int size);
} // namespace th06::Netplay
