#pragma once

#include "inttypes.hpp"

namespace th06
{
enum class SessionKind
{
    Local,
    Replay,
    LocalNetplay,
    Netplay,
    NetplayAuthoritative,
};

class ISession
{
public:
    virtual ~ISession() = default;

    virtual SessionKind Kind() const = 0;
    virtual void ResetInputState() = 0;
    virtual void AdvanceFrameInput() = 0;
};

namespace Session
{
void UseLocalSession();
void UseLocalNetplaySession();
void SetActiveSession(ISession &session);
ISession &GetActiveSession();
SessionKind GetKind();
bool IsDualPlayerSession();
bool IsRemoteNetplaySession();
void ResetInputState();
void AdvanceFrameInput();

// These helpers preserve the current legacy global input semantics so the
// session boundary can be introduced without rewriting downstream gameplay.
void ResetLegacyInputState();
void ApplyLegacyFrameInput(u16 nextInput);
}; // namespace Session
}; // namespace th06
