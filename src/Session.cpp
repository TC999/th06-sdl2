#include "Session.hpp"

#include "Controller.hpp"
#include "SinglePlayerSnapshot.hpp"
#include "Supervisor.hpp"

namespace th06
{
namespace
{
class LocalSession final : public ISession
{
public:
    SessionKind Kind() const override
    {
        return SessionKind::Local;
    }

    void ResetInputState() override
    {
        Controller::ResetDeviceInputState();
        Session::ResetLegacyInputState();
    }

    void AdvanceFrameInput() override
    {
        Session::ApplyLegacyFrameInput(SinglePlayerSnapshot::ProcessLocalGameplayInput(Controller::GetInput()));
    }
};

class LocalNetplaySession final : public ISession
{
public:
    SessionKind Kind() const override
    {
        return SessionKind::LocalNetplay;
    }

    void ResetInputState() override
    {
        Controller::ResetDeviceInputState();
        Session::ResetLegacyInputState();
    }

    void AdvanceFrameInput() override
    {
        Session::ApplyLegacyFrameInput(Controller::GetInput());
    }
};

LocalSession g_LocalSession;
LocalNetplaySession g_LocalNetplaySession;
ISession *g_ActiveSession = &g_LocalSession;
} // namespace

void Session::UseLocalSession()
{
    g_ActiveSession = &g_LocalSession;
}

void Session::UseLocalNetplaySession()
{
    g_ActiveSession = &g_LocalNetplaySession;
}

void Session::SetActiveSession(ISession &session)
{
    g_ActiveSession = &session;
}

ISession &Session::GetActiveSession()
{
    return *g_ActiveSession;
}

SessionKind Session::GetKind()
{
    return g_ActiveSession->Kind();
}

bool Session::IsDualPlayerSession()
{
    const SessionKind kind = GetKind();
    return kind == SessionKind::LocalNetplay || kind == SessionKind::Netplay;
}

bool Session::IsRemoteNetplaySession()
{
    return GetKind() == SessionKind::Netplay;
}

void Session::ResetInputState()
{
    g_ActiveSession->ResetInputState();
}

void Session::AdvanceFrameInput()
{
    g_ActiveSession->AdvanceFrameInput();
}

void Session::ResetLegacyInputState()
{
    g_LastFrameInput = 0;
    g_CurFrameInput = 0;
    g_IsEigthFrameOfHeldInput = 0;
    g_NumOfFramesInputsWereHeld = 0;
}

void Session::ApplyLegacyFrameInput(u16 nextInput)
{
    g_LastFrameInput = g_CurFrameInput;
    g_CurFrameInput = nextInput;
    g_IsEigthFrameOfHeldInput = 0;

    if (g_LastFrameInput == g_CurFrameInput)
    {
        if (0x1e <= g_NumOfFramesInputsWereHeld)
        {
            if (g_NumOfFramesInputsWereHeld % 8 == 0)
            {
                g_IsEigthFrameOfHeldInput = 1;
            }
            if (0x26 <= g_NumOfFramesInputsWereHeld)
            {
                g_NumOfFramesInputsWereHeld = 0x1e;
            }
        }
        g_NumOfFramesInputsWereHeld++;
    }
    else
    {
        g_NumOfFramesInputsWereHeld = 0;
    }
}
}; // namespace th06
