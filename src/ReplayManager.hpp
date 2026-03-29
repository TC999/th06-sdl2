#pragma once

#include "Chain.hpp"
#include "ChainPriorities.hpp"
#include "ReplayData.hpp"
#include "inttypes.hpp"

namespace th06
{
struct ReplayManager
{
    static ZunResult RegisterChain(i32 isDemo, char *replayFile);
    static ChainCallbackResult OnUpdate(ReplayManager *mgr);
    static ChainCallbackResult OnUpdateDemoHighPrio(ReplayManager *mgr);
    static ChainCallbackResult OnUpdateDemoLowPrio(ReplayManager *mgr);
    static ChainCallbackResult OnDraw(ReplayManager *mgr);
    static ZunResult AddedCallback(ReplayManager *mgr);
    static ZunResult AddedCallbackDemo(ReplayManager *mgr);
    static ZunResult DeletedCallback(ReplayManager *mgr);
    static void StopRecording();
    static void SaveReplay(char *replay_path, char *param_2);
    static ReplayData *LoadReplayData(char *replayFile, int isExternalResource);
    static ZunResult ValidateReplayData(ReplayData *data, i32 fileSize);

    ReplayManager()
    {
        int idx;
        frameId = 0;
        replayData = NULL;
        isDemo = 0;
        replayFile = NULL;
        for (idx = 0; idx < 52; ++idx)
        {
            unk10[idx] = 0;
        }
        unk44 = 0;
        replayInputs = NULL;
        replayHeldFrames = 0;
        replayTraceFlags = 0;
        for (idx = 0; idx < 7; ++idx)
        {
            replayInputStageBookmarks[idx] = NULL;
        }
        calcChain = NULL;
        drawChain = NULL;
        calcChainDemoHighPrio = NULL;
    }

    i32 IsDemo()
    {
        return this->isDemo;
    }

    i32 frameId;
    ReplayData *replayData;
    i32 isDemo;
    char *replayFile;
    u8 unk10[52];
    u16 unk44;
    ReplayDataInput *replayInputs;
    u16 replayHeldFrames;
    u8 replayTraceFlags;
    ReplayDataInput *replayInputStageBookmarks[7];
    ChainElem *calcChain;
    ChainElem *drawChain;
    ChainElem *calcChainDemoHighPrio;
};
}; // namespace th06
