#include "NetplayAuthoritativeReplicator.hpp"

#include "MainMenu.hpp"

namespace th06::Netplay::AuthoritativeReplicator
{
namespace
{
constexpr int kBulletArraySize = 640;
constexpr int kLaserArraySize = 64;
constexpr int kEnemyArraySize = 257;
constexpr int kItemArraySize = 513;

bool g_PreviousMirroredBullets[kBulletArraySize] {};
bool g_PreviousMirroredLasers[kLaserArraySize] {};
bool g_PreviousMirroredEnemies[kEnemyArraySize] {};
bool g_PreviousMirroredItems[kItemArraySize] {};

constexpr uint64_t kFNV64Offset = 1469598103934665603ull;
constexpr uint64_t kFNV64Prime = 1099511628211ull;

template <typename T> void HashValue(uint64_t &hash, const T &value)
{
    const u8 *bytes = (const u8 *)&value;
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        hash ^= bytes[i];
        hash *= kFNV64Prime;
    }
}

template <typename T> void HashBuffer(uint64_t &hash, const T *values, size_t count)
{
    if (values == nullptr || count == 0)
    {
        return;
    }
    const u8 *bytes = (const u8 *)values;
    const size_t byteCount = sizeof(T) * count;
    for (size_t i = 0; i < byteCount; ++i)
    {
        hash ^= bytes[i];
        hash *= kFNV64Prime;
    }
}

bool IsNearEitherPlayer(const D3DXVECTOR3 &position, float margin)
{
    const auto nearRect = [margin](const D3DXVECTOR3 &p) {
        return p.x >= (GAME_REGION_LEFT - margin) && p.x <= (GAME_REGION_RIGHT + margin) &&
               p.y >= (GAME_REGION_TOP - margin) && p.y <= (GAME_REGION_BOTTOM + margin);
    };
    if (nearRect(position))
    {
        return true;
    }
    const auto nearPlayer = [&position](const Player &player) {
        const float dx = position.x - player.positionCenter.x;
        const float dy = position.y - player.positionCenter.y;
        return dx * dx + dy * dy <= (220.0f * 220.0f);
    };
    return nearPlayer(g_Player) || (Session::IsDualPlayerSession() && nearPlayer(g_Player2));
}

u16 ReadRawEnemyFlags(const Enemy &enemy)
{
    u16 flags = 0;
    std::memcpy(&flags, &enemy.flags, std::min(sizeof(flags), sizeof(enemy.flags)));
    return flags;
}

void CapturePlayerState(const Player &player, bool isPlayer1, ReplicatedPlayerState &outState)
{
    outState = {};
    outState.positionCenter = player.positionCenter;
    outState.orbsPosition[0] = player.orbsPosition[0];
    outState.orbsPosition[1] = player.orbsPosition[1];
    outState.respawnTimer = player.respawnTimer;
    outState.playerState = player.playerState;
    outState.orbState = player.orbState;
    outState.isFocus = player.isFocus ? 1 : 0;
    const u16 laneShoot = isPlayer1 ? TH_BUTTON_SHOOT : TH_BUTTON_SHOOT2;
    outState.isShooting = (g_CurFrameInput & laneShoot) != 0 ? 1 : 0;
    outState.bombActive = player.bombInfo.isInUse != 0 ? 1 : 0;
}

template <typename TPlayerState> void ApplyPlayerState(Player &player, const TPlayerState &state)
{
    player.positionCenter = state.positionCenter;
    player.orbsPosition[0] = state.orbsPosition[0];
    player.orbsPosition[1] = state.orbsPosition[1];
    player.respawnTimer = state.respawnTimer;
    player.playerState = state.playerState;
    player.orbState = state.orbState;
    player.isFocus = state.isFocus != 0;

    player.hitboxTopLeft = player.positionCenter - player.hitboxSize;
    player.hitboxBottomRight = player.positionCenter + player.hitboxSize;
    player.grabItemTopLeft = player.positionCenter - player.grabItemSize;
    player.grabItemBottomRight = player.positionCenter + player.grabItemSize;
}

void CaptureHudState(ReplicatedHudState &outHud)
{
    outHud = {};
    outHud.guiScore = g_GameManager.guiScore;
    outHud.score = g_GameManager.score;
    outHud.highScore = g_GameManager.highScore;
    outHud.currentStage = g_GameManager.currentStage;
    outHud.grazeInStage = g_GameManager.grazeInStage;
    outHud.pointItemsCollectedInStage = g_GameManager.pointItemsCollectedInStage;
    outHud.currentPower = g_GameManager.currentPower;
    outHud.currentPower2 = g_GameManager.currentPower2;
    outHud.livesRemaining = g_GameManager.livesRemaining;
    outHud.bombsRemaining = g_GameManager.bombsRemaining;
    outHud.livesRemaining2 = g_GameManager.livesRemaining2;
    outHud.bombsRemaining2 = g_GameManager.bombsRemaining2;
    outHud.bossUIOpacity = g_Gui.bossUIOpacity;
    outHud.spellcardSecondsRemaining = g_Gui.spellcardSecondsRemaining;
    outHud.lastSpellcardSecondsRemaining = g_Gui.lastSpellcardSecondsRemaining;
    outHud.bossHealthBar1 = g_Gui.bossHealthBar1;
    outHud.bossHealthBar2 = g_Gui.bossHealthBar2;
}

void CaptureUiState(ReplicatedUiState &outUi)
{
    outUi = {};
    outUi.supervisorCurState = g_Supervisor.curState;
    outUi.supervisorWantedState = g_Supervisor.wantedState;
    outUi.mainMenuGameState = (i32)g_MainMenu.gameState;
    outUi.mainMenuCursor = g_MainMenu.cursor;
    outUi.mainMenuStateTimer = g_MainMenu.stateTimer;
    outUi.gameDifficulty = (i32)g_GameManager.difficulty;
    outUi.gameCharacter = g_GameManager.character;
    outUi.gameCharacter2 = g_GameManager.character2;
    outUi.gameShotType = g_GameManager.shotType;
    outUi.gameShotType2 = g_GameManager.shotType2;
    outUi.isInPracticeMode = g_GameManager.isInPracticeMode;
    outUi.menuCursorBackup = g_GameManager.menuCursorBackup;
}

uint32_t CaptureFrameFlags()
{
    uint32_t flags = 0;
    if (g_GameManager.isInGameMenu)
    {
        flags |= AuthoritativeFrameFlag_InGameMenu;
    }
    if (g_GameManager.isInRetryMenu)
    {
        flags |= AuthoritativeFrameFlag_InRetryMenu;
    }
    if (g_GameManager.isGameCompleted)
    {
        flags |= AuthoritativeFrameFlag_GameCompleted;
    }
    if (g_Gui.bossPresent)
    {
        flags |= AuthoritativeFrameFlag_BossPresent;
    }
    if (g_GameManager.isInPracticeMode)
    {
        flags |= AuthoritativeFrameFlag_InPractice;
    }
    if (Session::IsDualPlayerSession())
    {
        flags |= AuthoritativeFrameFlag_DualSession;
    }
    return flags;
}

void CaptureBullets(ReplicatedWorldState &outState)
{
    outState.bulletCount = 0;
    for (int i = 0; i < kBulletArraySize &&
                    outState.bulletCount < kAuthoritativeMaxReplicatedBullets;
         ++i)
    {
        const Bullet &bullet = g_BulletManager.bullets[i];
        if (bullet.state == 0 || !IsNearEitherPlayer(bullet.pos, 96.0f))
        {
            continue;
        }

        ReplicatedBulletState &dst = outState.bullets[outState.bulletCount++];
        dst.index = (u16)i;
        dst.spriteOffset = bullet.spriteOffset;
        dst.pos = bullet.pos;
        dst.velocity = bullet.velocity;
        dst.angle = bullet.angle;
        dst.radius = std::max({bullet.sprites.grazeSize.x, bullet.sprites.grazeSize.y, bullet.sprites.grazeSize.z});
        dst.state = bullet.state;
        dst.isGrazed = bullet.isGrazed;
        dst.provokedPlayer = bullet.provokedPlayer;
    }
}

void CaptureLasers(ReplicatedWorldState &outState)
{
    outState.laserCount = 0;
    for (int i = 0; i < kLaserArraySize &&
                    outState.laserCount < kAuthoritativeMaxReplicatedLasers;
         ++i)
    {
        const Laser &laser = g_BulletManager.lasers[i];
        if (laser.inUse == 0 || !IsNearEitherPlayer(laser.pos, 160.0f))
        {
            continue;
        }

        ReplicatedLaserState &dst = outState.lasers[outState.laserCount++];
        dst.index = (u16)i;
        dst.color = laser.color;
        dst.pos = laser.pos;
        dst.angle = laser.angle;
        dst.startOffset = laser.startOffset;
        dst.endOffset = laser.endOffset;
        dst.startLength = laser.startLength;
        dst.width = laser.width;
        dst.speed = laser.speed;
        dst.startTime = laser.startTime;
        dst.hitboxStartTime = laser.hitboxStartTime;
        dst.duration = laser.duration;
        dst.despawnDuration = laser.despawnDuration;
        dst.hitboxEndDelay = laser.hitboxEndDelay;
        dst.inUse = laser.inUse;
        dst.flags = laser.flags;
        dst.state = laser.state;
        dst.provokedPlayer = laser.provokedPlayer;
    }
}

void CaptureEnemies(ReplicatedWorldState &outState)
{
    outState.enemyCount = 0;
    for (int i = 0; i < kEnemyArraySize &&
                    outState.enemyCount < kAuthoritativeMaxReplicatedEnemies;
         ++i)
    {
        const Enemy &enemy = g_EnemyManager.enemies[i];
        if (!enemy.flags.unk5)
        {
            continue;
        }
        if (!enemy.flags.isBoss && !IsNearEitherPlayer(enemy.position, 192.0f))
        {
            continue;
        }

        ReplicatedEnemyState &dst = outState.enemies[outState.enemyCount++];
        dst.index = (u16)i;
        dst.active = enemy.flags.unk5 ? 1 : 0;
        dst.isBoss = enemy.flags.isBoss ? 1 : 0;
        dst.itemDrop = enemy.itemDrop;
        dst.bossId = enemy.bossId;
        dst.position = enemy.position;
        dst.hitboxDimensions = enemy.hitboxDimensions;
        dst.angle = enemy.angle;
        dst.life = enemy.life;
        dst.maxLife = enemy.maxLife;
        dst.score = enemy.score;
        dst.currentSubId = enemy.currentContext.subId;
        dst.flags = ReadRawEnemyFlags(enemy);
    }
}

void CaptureItems(ReplicatedWorldState &outState)
{
    outState.itemCount = 0;
    for (int i = 0; i < kItemArraySize &&
                    outState.itemCount < kAuthoritativeMaxReplicatedItems;
         ++i)
    {
        const Item &item = g_ItemManager.items[i];
        if (!item.isInUse || !IsNearEitherPlayer(item.currentPosition, 128.0f))
        {
            continue;
        }

        ReplicatedItemState &dst = outState.items[outState.itemCount++];
        dst.index = (u16)i;
        dst.itemType = item.itemType;
        dst.currentPosition = item.currentPosition;
        dst.targetPosition = item.targetPosition;
        dst.state = item.state;
        dst.isInUse = item.isInUse;
    }
}

uint64_t ComputeReplicatedWorldDigest(const ReplicatedWorldState &state)
{
    uint64_t hash = kFNV64Offset;
    HashValue(hash, state.serverFrame);
    HashValue(hash, state.ackedInputFrameP1);
    HashValue(hash, state.ackedInputFrameP2);
    HashValue(hash, state.flags);
    HashValue(hash, state.bgmCue);
    HashValue(hash, state.player1);
    HashValue(hash, state.player2);
    HashValue(hash, state.hud);
    HashValue(hash, state.ui);
    HashBuffer(hash, state.bgmPath, sizeof(state.bgmPath));
    HashBuffer(hash, state.posPath, sizeof(state.posPath));
    HashValue(hash, state.bgmIsLooping);
    HashValue(hash, state.bulletCount);
    HashValue(hash, state.laserCount);
    HashValue(hash, state.enemyCount);
    HashValue(hash, state.itemCount);
    HashBuffer(hash, state.bullets, state.bulletCount);
    HashBuffer(hash, state.lasers, state.laserCount);
    HashBuffer(hash, state.enemies, state.enemyCount);
    HashBuffer(hash, state.items, state.itemCount);
    return hash;
}

bool NeedsBgmReload(const AuthoritativeFrameState &state)
{
    const bool hasAuthoritativeBgm = state.bgmPath[0] != '\0';
    const bool hasCurrentBgm = g_SoundPlayer.currentBgmPath[0] != '\0';
    if (hasAuthoritativeBgm != hasCurrentBgm)
    {
        return true;
    }
    if (!hasAuthoritativeBgm)
    {
        return false;
    }
    if (std::strcmp(g_SoundPlayer.currentBgmPath, state.bgmPath) != 0)
    {
        return true;
    }
    if (std::strcmp(g_SoundPlayer.currentPosPath, state.posPath) != 0)
    {
        return true;
    }
    if (g_SoundPlayer.isLooping != state.bgmIsLooping)
    {
        return true;
    }
    return g_SoundPlayer.backgroundMusic == nullptr;
}

void ApplyBgmCue(const AuthoritativeFrameState &state)
{
    if (!NeedsBgmReload(state))
    {
        return;
    }

    if (state.bgmPath[0] == '\0')
    {
        g_SoundPlayer.StopBGM();
        g_SoundPlayer.currentBgmPath[0] = '\0';
        g_SoundPlayer.currentPosPath[0] = '\0';
        return;
    }

    if (g_SoundPlayer.LoadWav(const_cast<char *>(state.bgmPath)) != ZUN_SUCCESS)
    {
        return;
    }
    if (state.posPath[0] != '\0')
    {
        g_SoundPlayer.LoadPos(const_cast<char *>(state.posPath));
    }
    g_SoundPlayer.PlayBGM(state.bgmIsLooping);
}

void ApplyHudState(const ReplicatedHudState &hud, uint32_t flags)
{
    g_GameManager.guiScore = hud.guiScore;
    g_GameManager.score = hud.score;
    g_GameManager.highScore = hud.highScore;
    g_GameManager.currentStage = hud.currentStage;
    g_GameManager.grazeInStage = hud.grazeInStage;
    g_GameManager.pointItemsCollectedInStage = hud.pointItemsCollectedInStage;
    g_GameManager.currentPower = hud.currentPower;
    g_GameManager.currentPower2 = hud.currentPower2;
    g_GameManager.livesRemaining = hud.livesRemaining;
    g_GameManager.bombsRemaining = hud.bombsRemaining;
    g_GameManager.livesRemaining2 = hud.livesRemaining2;
    g_GameManager.bombsRemaining2 = hud.bombsRemaining2;
    g_GameManager.isInGameMenu = (flags & AuthoritativeFrameFlag_InGameMenu) != 0;
    g_GameManager.isInRetryMenu = (flags & AuthoritativeFrameFlag_InRetryMenu) != 0;
    g_GameManager.isGameCompleted = (flags & AuthoritativeFrameFlag_GameCompleted) != 0;
    g_GameManager.isInPracticeMode = (flags & AuthoritativeFrameFlag_InPractice) != 0;

    g_Gui.bossPresent = (flags & AuthoritativeFrameFlag_BossPresent) != 0;
    g_Gui.bossUIOpacity = hud.bossUIOpacity;
    g_Gui.spellcardSecondsRemaining = hud.spellcardSecondsRemaining;
    g_Gui.lastSpellcardSecondsRemaining = hud.lastSpellcardSecondsRemaining;
    g_Gui.bossHealthBar1 = hud.bossHealthBar1;
    g_Gui.bossHealthBar2 = hud.bossHealthBar2;
}

void ApplyUiState(const ReplicatedUiState &ui)
{
    g_Supervisor.curState = ui.supervisorCurState;
    g_Supervisor.wantedState = ui.supervisorWantedState;
    g_MainMenu.gameState = (GameState)ui.mainMenuGameState;
    g_MainMenu.cursor = ui.mainMenuCursor;
    g_MainMenu.stateTimer = ui.mainMenuStateTimer;

    g_GameManager.difficulty = (Difficulty)ui.gameDifficulty;
    g_GameManager.character = ui.gameCharacter;
    g_GameManager.character2 = ui.gameCharacter2;
    g_GameManager.shotType = ui.gameShotType;
    g_GameManager.shotType2 = ui.gameShotType2;
    g_GameManager.isInPracticeMode = ui.isInPracticeMode;
    g_GameManager.menuCursorBackup = ui.menuCursorBackup;
}

void ApplyBullets(const AuthoritativeFrameState &state)
{
    bool currentMirrored[kBulletArraySize] {};
    for (u16 i = 0; i < state.bulletCount; ++i)
    {
        const ReplicatedBulletState &src = state.bullets[i];
        if (src.index >= kBulletArraySize)
        {
            continue;
        }
        Bullet &dst = g_BulletManager.bullets[src.index];
        dst.pos = src.pos;
        dst.velocity = src.velocity;
        dst.speed = std::sqrt(src.velocity.x * src.velocity.x + src.velocity.y * src.velocity.y);
        dst.angle = src.angle;
        dst.spriteOffset = src.spriteOffset;
        dst.state = src.state;
        dst.isGrazed = src.isGrazed;
        dst.provokedPlayer = src.provokedPlayer;
        if (src.radius > 0.0f)
        {
            dst.sprites.grazeSize = D3DXVECTOR3(src.radius, src.radius, src.radius);
        }
        currentMirrored[src.index] = true;
    }

    for (int i = 0; i < kBulletArraySize; ++i)
    {
        if (g_PreviousMirroredBullets[i] && !currentMirrored[i])
        {
            g_BulletManager.bullets[i].state = 0;
        }
        g_PreviousMirroredBullets[i] = currentMirrored[i];
    }
    g_BulletManager.bulletCount = state.bulletCount;
}

void ApplyLasers(const AuthoritativeFrameState &state)
{
    bool currentMirrored[kLaserArraySize] {};
    for (u16 i = 0; i < state.laserCount; ++i)
    {
        const ReplicatedLaserState &src = state.lasers[i];
        if (src.index >= kLaserArraySize)
        {
            continue;
        }
        Laser &dst = g_BulletManager.lasers[src.index];
        dst.pos = src.pos;
        dst.angle = src.angle;
        dst.startOffset = src.startOffset;
        dst.endOffset = src.endOffset;
        dst.startLength = src.startLength;
        dst.width = src.width;
        dst.speed = src.speed;
        dst.startTime = src.startTime;
        dst.hitboxStartTime = src.hitboxStartTime;
        dst.duration = src.duration;
        dst.despawnDuration = src.despawnDuration;
        dst.hitboxEndDelay = src.hitboxEndDelay;
        dst.inUse = src.inUse;
        dst.flags = src.flags;
        dst.color = src.color;
        dst.state = src.state;
        dst.provokedPlayer = src.provokedPlayer;
        currentMirrored[src.index] = true;
    }

    for (int i = 0; i < kLaserArraySize; ++i)
    {
        if (g_PreviousMirroredLasers[i] && !currentMirrored[i])
        {
            g_BulletManager.lasers[i].inUse = 0;
        }
        g_PreviousMirroredLasers[i] = currentMirrored[i];
    }
}

void ApplyEnemies(const AuthoritativeFrameState &state)
{
    bool currentMirrored[kEnemyArraySize] {};
    std::memset(g_EnemyManager.bosses, 0, sizeof(g_EnemyManager.bosses));
    int activeCount = 0;
    for (u16 i = 0; i < state.enemyCount; ++i)
    {
        const ReplicatedEnemyState &src = state.enemies[i];
        if (src.index >= kEnemyArraySize)
        {
            continue;
        }
        Enemy &dst = g_EnemyManager.enemies[src.index];
        dst.flags.unk5 = src.active != 0;
        dst.flags.isBoss = src.isBoss != 0;
        dst.position = src.position;
        dst.hitboxDimensions = src.hitboxDimensions;
        dst.angle = src.angle;
        dst.life = src.life;
        dst.maxLife = src.maxLife;
        dst.score = src.score;
        dst.itemDrop = src.itemDrop;
        dst.bossId = src.bossId;
        dst.currentContext.subId = src.currentSubId;
        currentMirrored[src.index] = true;
        if (dst.flags.unk5)
        {
            ++activeCount;
        }
        if (dst.flags.isBoss && dst.bossId < 8)
        {
            g_EnemyManager.bosses[dst.bossId] = &dst;
        }
    }

    for (int i = 0; i < kEnemyArraySize; ++i)
    {
        if (g_PreviousMirroredEnemies[i] && !currentMirrored[i])
        {
            g_EnemyManager.enemies[i].flags.unk5 = 0;
            g_EnemyManager.enemies[i].life = 0;
        }
        g_PreviousMirroredEnemies[i] = currentMirrored[i];
    }
    g_EnemyManager.enemyCount = activeCount;
}

void ApplyItems(const AuthoritativeFrameState &state)
{
    bool currentMirrored[kItemArraySize] {};
    u32 activeCount = 0;
    for (u16 i = 0; i < state.itemCount; ++i)
    {
        const ReplicatedItemState &src = state.items[i];
        if (src.index >= kItemArraySize)
        {
            continue;
        }
        Item &dst = g_ItemManager.items[src.index];
        dst.currentPosition = src.currentPosition;
        dst.targetPosition = src.targetPosition;
        dst.itemType = (i8)src.itemType;
        dst.state = src.state;
        dst.isInUse = src.isInUse;
        currentMirrored[src.index] = true;
        if (dst.isInUse)
        {
            ++activeCount;
        }
    }

    for (int i = 0; i < kItemArraySize; ++i)
    {
        if (g_PreviousMirroredItems[i] && !currentMirrored[i])
        {
            g_ItemManager.items[i].isInUse = 0;
        }
        g_PreviousMirroredItems[i] = currentMirrored[i];
    }
    g_ItemManager.itemCount = activeCount;
}
} // namespace

void Reset()
{
    g_State.authoritativeFrameHashes.clear();
    g_State.latestAuthoritativeFrameState = {};
    g_State.lastAuthoritativeHashComparedFrame = -1;
    g_State.authoritativeHashMismatchPending = false;
    std::memset(g_PreviousMirroredBullets, 0, sizeof(g_PreviousMirroredBullets));
    std::memset(g_PreviousMirroredLasers, 0, sizeof(g_PreviousMirroredLasers));
    std::memset(g_PreviousMirroredEnemies, 0, sizeof(g_PreviousMirroredEnemies));
    std::memset(g_PreviousMirroredItems, 0, sizeof(g_PreviousMirroredItems));
}

bool CaptureCurrentReplicatedWorldState(int serverFrame, ReplicatedWorldState &outState)
{
    outState = {};
    outState.valid = true;
    outState.serverFrame = serverFrame;
    outState.ackedInputFrameP1 = std::max(0, serverFrame - g_State.delay);
    outState.ackedInputFrameP2 = std::max(0, serverFrame - g_State.delay);
    outState.flags = CaptureFrameFlags();
    outState.bgmCue = 0;
    CapturePlayerState(g_Player, true, outState.player1);
    CapturePlayerState(g_Player2, false, outState.player2);
    CaptureHudState(outState.hud);
    CaptureUiState(outState.ui);
    std::memcpy(outState.bgmPath, g_SoundPlayer.currentBgmPath, sizeof(outState.bgmPath));
    std::memcpy(outState.posPath, g_SoundPlayer.currentPosPath, sizeof(outState.posPath));
    outState.bgmIsLooping = g_SoundPlayer.isLooping;
    CaptureBullets(outState);
    CaptureLasers(outState);
    CaptureEnemies(outState);
    CaptureItems(outState);
    outState.receivedActorMask = outState.expectedActorMask;
    outState.worldHash = ComputeReplicatedWorldDigest(outState);
    g_State.authoritativeFrameHashes[serverFrame] = outState.worldHash;
    return true;
}

void ApplyLatestAuthoritativeStateToLiveWorld()
{
    if (g_State.isHost || !g_State.latestAuthoritativeFrameState.valid)
    {
        return;
    }

    ApplyPlayerState(g_Player, g_State.latestAuthoritativeFrameState.player1);
    ApplyPlayerState(g_Player2, g_State.latestAuthoritativeFrameState.player2);
    ApplyHudState(g_State.latestAuthoritativeFrameState.hud, g_State.latestAuthoritativeFrameState.flags);
    ApplyUiState(g_State.latestAuthoritativeFrameState.ui);
    ApplyBullets(g_State.latestAuthoritativeFrameState);
    ApplyLasers(g_State.latestAuthoritativeFrameState);
    ApplyEnemies(g_State.latestAuthoritativeFrameState);
    ApplyItems(g_State.latestAuthoritativeFrameState);
    ApplyBgmCue(g_State.latestAuthoritativeFrameState);
}

void RecordLocalMirrorFrameHash(int frame)
{
    ReplicatedWorldState state {};
    const int captureFrame =
        (!g_State.isHost && g_State.latestAuthoritativeFrameState.valid) ? g_State.latestAuthoritativeFrameState.serverFrame
                                                                         : frame;
    if (!CaptureCurrentReplicatedWorldState(captureFrame, state))
    {
        return;
    }

    g_State.authoritativeFrameHashes[captureFrame] = state.worldHash;
}

bool TryConsumeHostMismatchFrame(int &outMismatchFrame)
{
    outMismatchFrame = -1;
    if (g_State.isHost || !g_State.latestAuthoritativeFrameState.valid)
    {
        return false;
    }

    const int compareFrame = g_State.latestAuthoritativeFrameState.serverFrame;
    if (compareFrame <= g_State.lastAuthoritativeHashComparedFrame)
    {
        return false;
    }

    if (!g_State.authoritativeHashCheckEnabled)
    {
        g_State.lastAuthoritativeHashComparedFrame = compareFrame;
        g_State.authoritativeHashMismatchPending = false;
        TraceDiagnostic("authoritative-hash-compare-bypass",
                        "frame=%d reason=state-not-complete actorMask=%u expected=%u", compareFrame,
                        g_State.latestAuthoritativeFrameState.receivedActorMask,
                        g_State.latestAuthoritativeFrameState.expectedActorMask);
        return false;
    }

    const auto it = g_State.authoritativeFrameHashes.find(compareFrame);
    if (it == g_State.authoritativeFrameHashes.end())
    {
        return false;
    }

    g_State.lastAuthoritativeHashComparedFrame = compareFrame;
    if (it->second == g_State.latestAuthoritativeFrameState.worldHash)
    {
        g_State.authoritativeHashMismatchPending = false;
        TraceDiagnostic("authoritative-hash-match", "frame=%d local=%llu remote=%llu", compareFrame,
                        (unsigned long long)it->second,
                        (unsigned long long)g_State.latestAuthoritativeFrameState.worldHash);
        return false;
    }

    g_State.authoritativeHashMismatchPending = true;
    outMismatchFrame = compareFrame;
    TraceDiagnostic("authoritative-hash-mismatch", "frame=%d local=%llu remote=%llu", compareFrame,
                    (unsigned long long)it->second,
                    (unsigned long long)g_State.latestAuthoritativeFrameState.worldHash);
    return true;
}
} // namespace th06::Netplay::AuthoritativeReplicator
