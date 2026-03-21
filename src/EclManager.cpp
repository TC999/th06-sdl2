#include "EclManager.hpp"
#include "AnmManager.hpp"
#include "EffectManager.hpp"
#include "Enemy.hpp"
#include "EnemyEclInstr.hpp"
#include "EnemyManager.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "Session.hpp"
#include "Stage.hpp"
#include "utils.hpp"
#include "thprac_th06.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace th06
{
DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 64, g_SpellcardScore) = {
    200000, 200000, 200000, 200000, 200000, 200000, 200000, 250000, 250000, 250000, 250000, 250000, 250000,
    250000, 300000, 300000, 300000, 300000, 300000, 300000, 300000, 300000, 300000, 300000, 300000, 300000,
    300000, 300000, 300000, 300000, 300000, 300000, 400000, 400000, 400000, 400000, 400000, 400000, 400000,
    400000, 500000, 500000, 500000, 500000, 500000, 500000, 600000, 600000, 600000, 600000, 600000, 700000,
    700000, 700000, 700000, 700000, 700000, 700000, 700000, 700000, 700000, 700000, 700000, 700000};
DIFFABLE_STATIC(EclManager, g_EclManager);
typedef void (*ExInsn)(Enemy *, EclRawInstr *);
DIFFABLE_STATIC_ARRAY_ASSIGN(ExInsn, 17, g_EclExInsn) = {
    EnemyEclInstr::ExInsCirnoRainbowBallJank, EnemyEclInstr::ExInsShootAtRandomArea,
    EnemyEclInstr::ExInsShootStarPattern,     EnemyEclInstr::ExInsPatchouliShottypeSetVars,
    EnemyEclInstr::ExInsStage56Func4,         EnemyEclInstr::ExInsStage5Func5,
    EnemyEclInstr::ExInsStage6XFunc6,         EnemyEclInstr::ExInsStage6Func7,
    EnemyEclInstr::ExInsStage6Func8,          EnemyEclInstr::ExInsStage6Func9,
    EnemyEclInstr::ExInsStage6XFunc10,        EnemyEclInstr::ExInsStage6Func11,
    EnemyEclInstr::ExInsStage4Func12,         EnemyEclInstr::ExInsStageXFunc13,
    EnemyEclInstr::ExInsStageXFunc14,         EnemyEclInstr::ExInsStageXFunc15,
    EnemyEclInstr::ExInsStageXFunc16};

namespace
{
bool HasSecondPlayer()
{
    return Session::IsDualPlayerSession();
}
} // namespace

static inline void TraceSpellcardOpcode(Enemy *enemy, const EclRawInstr *rawInstruction)
{
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
    if (!enemy->flags.isBoss || !g_EnemyManager.spellcardInfo.isActive)
    {
        return;
    }

    const u8 *rawInstructionBytes = reinterpret_cast<const u8 *>(rawInstruction);
    const i32 instructionTime = utils::ReadUnaligned<i32>(rawInstructionBytes + offsetof(EclRawInstr, time));
    const i16 opCode = utils::ReadUnaligned<i16>(rawInstructionBytes + offsetof(EclRawInstr, opCode));
    const i16 offsetToNext = utils::ReadUnaligned<i16>(rawInstructionBytes + offsetof(EclRawInstr, offsetToNext));
    SDL_Log("[spell/ecl] boss=%d spell=%d sub=%u time=%d insnTime=%d op=%d next=%d stack=%d cmp=%d", enemy->bossId,
            g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId, enemy->currentContext.time.current,
            instructionTime, opCode, offsetToNext, enemy->stackDepth, enemy->currentContext.compareRegister);
#endif
}

ZunResult EclManager::Load(char *eclPath)
{
    i32 idx;

    this->eclFile = (EclRawHeader *)FileSystem::OpenPath(eclPath, false);
    if (this->eclFile == NULL)
    {
        GameErrorContext::Log(&g_GameErrorContext, TH_ERR_ECLMANAGER_ENEMY_DATA_CORRUPT);
        return ZUN_ERROR;
    }
    this->eclFile->timelineOffsets[0] =
        (EclTimelineInstr *)((int)this->eclFile->timelineOffsets[0] + (int)this->eclFile);
    this->subTable = &this->eclFile->subOffsets[0];
    for (idx = 0; idx < this->eclFile->subCount; idx++)
    {
        this->subTable[idx] = (EclRawInstr *)((int)this->subTable[idx] + (int)this->eclFile);
    }
    this->timeline = this->eclFile->timelineOffsets[0];
    return ZUN_SUCCESS;
}

void EclManager::Unload()
{
    EclRawHeader *file;

    if (this->eclFile != NULL)
    {
        file = this->eclFile;
        free(file);
    }
    this->eclFile = NULL;
    return;
}

ZunResult EclManager::CallEclSub(EnemyEclContext *ctx, i16 subId)
{
    ctx->currentInstr = this->subTable[subId];
    ctx->time.InitializeForPopup();
    ctx->subId = subId;
    return ZUN_SUCCESS;
}

f32 EclManager::AngleProvokedPlayer(D3DXVECTOR3 *pos, u8 playerType)
{
    if (playerType == 2)
    {
        return g_Player2.AngleToPlayer(pos);
    }
    return g_Player.AngleToPlayer(pos);
}

#pragma var_order(local_8, local_14, local_18, args, instruction, local_24, local_28, local_2c, local_30, local_34,    \
                  local_38, local_3c, local_40, local_44, local_48, local_4c, local_50, local_54, local_58, local_5c,  \
                  local_60, local_64, local_68, local_6c, local_70, local_74, csum, scoreIncrease, local_80, local_84, \
                  local_88, local_8c, local_98, local_b0, local_b4, local_b8, local_bc, local_c0)
ZunResult EclManager::RunEcl(Enemy *enemy)
{
    EclRawInstr *instruction;
    EclRawInstr *rawInstruction;
    EclRawInstrArgs *args;
    const u8 *rawInstructionBytes;
    i16 rawOffsetToNext;
    ZunVec3 local_8;
    i32 local_14, local_24, local_28, local_2c, *local_3c, *local_40, local_44, local_48, local_68, local_74, csum,
        scoreIncrease, local_84, local_88, local_8c, local_b8, local_c0;
    f32 local_18, local_30, local_34, local_38, local_4c, local_50, local_bc;
    Catk *local_70, *local_80;
    EclRawInstrBulletArgs *local_54;
    EnemyBulletShooter *local_58;
    EclRawInstrAnmSetDeathArgs *local_5c;
    EnemyLaserShooter *local_60;
    EclRawInstrLaserArgs *local_64;
    EclRawInstrSpellcardEffectArgs *local_6c;
    D3DXVECTOR3 local_98;
    EclRawInstrEnemyCreateArgs local_b0;
    Enemy *local_b4;
    std::vector<u8> alignedInstructionStorage;
    const bool hasSecondPlayer = HasSecondPlayer();

    auto updateRawInstructionMetadata = [&]() -> ZunResult {
        rawInstructionBytes = reinterpret_cast<const u8 *>(rawInstruction);
        rawOffsetToNext = utils::ReadUnaligned<i16>(rawInstructionBytes + offsetof(EclRawInstr, offsetToNext));
        if (rawOffsetToNext <= 0)
        {
            return ZUN_ERROR;
        }
        return ZUN_SUCCESS;
    };
    auto loadAlignedInstruction = [&]() -> ZunResult {
        if (updateRawInstructionMetadata() != ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }

        const size_t instructionSize = (size_t)(u16)rawOffsetToNext;
        const size_t instructionAlignment = alignof(EclRawInstr);
        alignedInstructionStorage.resize(instructionSize + instructionAlignment - 1);

        // std::vector<u8> is only byte-aligned, so align the scratch copy before typed field access.
        u8 *storageBegin = alignedInstructionStorage.data();
        uintptr_t storageAddress = reinterpret_cast<uintptr_t>(storageBegin);
        uintptr_t alignedAddress = (storageAddress + instructionAlignment - 1) & ~(instructionAlignment - 1);
        instruction = reinterpret_cast<EclRawInstr *>(alignedAddress);

        memcpy(instruction, rawInstructionBytes, instructionSize);
        args = &instruction->args;
        return ZUN_SUCCESS;
    };
#define RAW_ARG_PTR(type, args_type, member)                                                                                  \
    reinterpret_cast<type *>(const_cast<u8 *>(rawInstructionBytes + offsetof(EclRawInstr, args) + offsetof(args_type, member)))

    for (;;)
    {
        rawInstruction = enemy->currentContext.currentInstr;
        if (updateRawInstructionMetadata() != ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }
        if (0 <= enemy->runInterrupt)
        {
            goto HANDLE_INTERRUPT;
        }

    YOLO:
        if (loadAlignedInstruction() != ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }
        if ((ZunBool)(enemy->currentContext.time.current == instruction->time))
        {
            if (!(instruction->skipForDifficulty & (1 << g_GameManager.difficulty)))
            {
                goto NEXT_INSN;
            }

            TraceSpellcardOpcode(enemy, rawInstruction);
            if (hasSecondPlayer && enemy->provokedPlayer == 0)
            {
                if (g_Player.RangeToPlayer(&enemy->position) > g_Player2.RangeToPlayer(&enemy->position))
                {
                    enemy->provokedPlayer = 2;
                }
                else
                {
                    enemy->provokedPlayer = 1;
                }
            }
            switch (instruction->opCode)
            {
            case ECL_OPCODE_UNIMP:
                return ZUN_ERROR;
            case ECL_OPCODE_JUMPDEC:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrJumpArgs, var), NULL);
                local_14--;
                EnemyEclInstr::SetVar(enemy, args->jump.var, &local_14);
                if (local_14 <= 0)
                    break;
            case ECL_OPCODE_JUMP:
            HANDLE_JUMP:
                enemy->currentContext.time.current = instruction->args.jump.time;
                rawInstruction = reinterpret_cast<EclRawInstr *>(const_cast<u8 *>(rawInstructionBytes + args->jump.offset));
                goto YOLO;
            case ECL_OPCODE_SETINT:
            case ECL_OPCODE_SETFLOAT:
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, RAW_ARG_PTR(i32, EclRawInstrAluArgs, arg1.i32));
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    const i32 *outValue = EnemyEclInstr::GetVar(enemy, &instruction->args.alu.res, NULL);
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d res=%d out_i=%d out_f=%f",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, instruction->args.alu.res, *outValue,
                            *(const f32 *)outValue);
                }
#endif
                break;
            case ECL_OPCODE_MATHNORMANGLE:
                local_18 = *(f32 *)EnemyEclInstr::GetVar(enemy, &instruction->args.alu.res, NULL);
                local_18 = utils::AddNormalizeAngle(local_18, 0.0f);
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &local_18);
                break;
            case ECL_OPCODE_SETINTRAND:
                local_24 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id), NULL);
                local_14 = g_Rng.GetRandomU32InRange(local_24);
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &local_14);
                break;
            case ECL_OPCODE_SETINTRANDMIN:
                local_28 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id), NULL);
                local_2c = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id), NULL);
                local_14 = g_Rng.GetRandomU32InRange(local_28);
                local_14 += local_2c;
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &local_14);
                break;
            case ECL_OPCODE_SETFLOATRAND:
                local_30 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg1.f32), NULL);
                local_18 = g_Rng.GetRandomF32InRange(local_30);
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &local_18);
                break;
            case ECL_OPCODE_SETFLOATRANDMIN:
                local_34 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg1.f32), NULL);
                local_38 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg2.f32), NULL);
                local_18 = g_Rng.GetRandomF32InRange(local_34);
                local_18 += local_38;
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &local_18);
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d res=%d range=%f base=%f out=%f",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, instruction->args.alu.res, local_34,
                            local_38, local_18);
                }
#endif
                break;
            case ECL_OPCODE_SETVARSELFX:
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &enemy->position.x);
                break;
            case ECL_OPCODE_SETVARSELFY:
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &enemy->position.y);
                break;
            case ECL_OPCODE_SETVARSELFZ:
                EnemyEclInstr::SetVar(enemy, instruction->args.alu.res, &enemy->position.z);
                break;
            case ECL_OPCODE_MATHINTADD:
            case ECL_OPCODE_MATHFLOATADD:
                EnemyEclInstr::MathAdd(enemy, instruction->args.alu.res,
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id),
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id));
                break;
            case ECL_OPCODE_MATHINC:
                local_3c = EnemyEclInstr::GetVar(enemy, &instruction->args.alu.res, NULL);
                *local_3c += 1;
                break;
            case ECL_OPCODE_MATHDEC:
                local_40 = EnemyEclInstr::GetVar(enemy, &instruction->args.alu.res, NULL);
                *local_40 -= 1;
                break;
            case ECL_OPCODE_MATHINTSUB:
            case ECL_OPCODE_MATHFLOATSUB:
                EnemyEclInstr::MathSub(enemy, instruction->args.alu.res,
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id),
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id));
                break;
            case ECL_OPCODE_MATHINTMUL:
            case ECL_OPCODE_MATHFLOATMUL:
                EnemyEclInstr::MathMul(enemy, instruction->args.alu.res,
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id),
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id));
                break;
            case ECL_OPCODE_MATHINTDIV:
            case ECL_OPCODE_MATHFLOATDIV:
                EnemyEclInstr::MathDiv(enemy, instruction->args.alu.res,
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id),
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id));
                break;
            case ECL_OPCODE_MATHINTMOD:
            case ECL_OPCODE_MATHFLOATMOD:
                EnemyEclInstr::MathMod(enemy, instruction->args.alu.res,
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg1.id),
                                       RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, arg2.id));
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    const i32 *outValue = EnemyEclInstr::GetVar(enemy, &instruction->args.alu.res, NULL);
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d res=%d out_i=%d out_f=%f",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, instruction->args.alu.res, *outValue,
                            *(const f32 *)outValue);
                }
#endif
                break;
            case ECL_OPCODE_MATHATAN2:
                EnemyEclInstr::MathAtan2(enemy, instruction->args.alu.res,
                                         RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg1.f32),
                                         RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg2.f32),
                                         RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg3.f32),
                                         RAW_ARG_PTR(f32, EclRawInstrAluArgs, arg4.f32));
                break;
            case ECL_OPCODE_CMPINT:
                local_48 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCmpArgs, lhs.id), NULL);
                local_44 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCmpArgs, rhs.id), NULL);
                enemy->currentContext.compareRegister = local_48 == local_44 ? 0 : local_48 < local_44 ? -1 : 1;
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d lhs=%d rhs=%d result=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, local_48, local_44,
                            enemy->currentContext.compareRegister);
                }
#endif
                break;
            case ECL_OPCODE_CMPFLOAT:
                local_4c = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrCmpArgs, lhs.f32), NULL);
                local_50 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrCmpArgs, rhs.f32), NULL);
                enemy->currentContext.compareRegister = local_4c == local_50 ? 0 : (local_4c < local_50 ? -1 : 1);
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d lhs=%f rhs=%f result=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, local_4c, local_50,
                            enemy->currentContext.compareRegister);
                }
#endif
                break;
            case ECL_OPCODE_JUMPLSS:
                if (enemy->currentContext.compareRegister < 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_JUMPLEQ:
                if (enemy->currentContext.compareRegister <= 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_JUMPEQU:
                if (enemy->currentContext.compareRegister == 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_JUMPGRE:
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d cmp=%d target_time=%d target_off=%d taken=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, enemy->currentContext.compareRegister,
                            instruction->args.jump.time, instruction->args.jump.offset,
                            enemy->currentContext.compareRegister > 0 ? 1 : 0);
                }
#endif
                if (enemy->currentContext.compareRegister > 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_JUMPGEQ:
                if (enemy->currentContext.compareRegister >= 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_JUMPNEQ:
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d cmp=%d target_time=%d target_off=%d taken=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, enemy->currentContext.compareRegister,
                            instruction->args.jump.time, instruction->args.jump.offset,
                            enemy->currentContext.compareRegister != 0 ? 1 : 0);
                }
#endif
                if (enemy->currentContext.compareRegister != 0)
                    goto HANDLE_JUMP;
                break;
            case ECL_OPCODE_CALL:
            HANDLE_CALL:
                local_14 = instruction->args.call.eclSub;
                enemy->currentContext.currentInstr =
                    reinterpret_cast<EclRawInstr *>(const_cast<u8 *>(rawInstructionBytes + rawOffsetToNext));
                if (enemy->flags.unk14 == 0)
                {
                    memcpy(&enemy->savedContextStack[enemy->stackDepth], &enemy->currentContext,
                           sizeof(EnemyEclContext));
                }
                g_EclManager.CallEclSub(&enemy->currentContext, (u16)local_14);
                if (enemy->flags.unk14 == 0 && enemy->stackDepth < 7)
                {
                    enemy->stackDepth++;
                }
                enemy->currentContext.var0 = instruction->args.call.var0;
                enemy->currentContext.float0 =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrCallArgs, float0), NULL);
                continue;
            case ECL_OPCODE_RET:
                if (enemy->flags.unk14)
                {
                    utils::DebugPrint2("error : no Stack Ret\n");
                }
                enemy->stackDepth--;
                memcpy(&enemy->currentContext, &enemy->savedContextStack[enemy->stackDepth], sizeof(EnemyEclContext));
                continue;
            case ECL_OPCODE_CALLLSS:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 < args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_CALLLEQ:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 <= args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_CALLEQU:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 == args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_CALLGRE:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 > args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_CALLGEQ:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 >= args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_CALLNEQ:
                local_14 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrCallArgs, cmpLhs), NULL);
                if (local_14 != args->call.cmpRhs)
                    goto HANDLE_CALL;
                break;
            case ECL_OPCODE_ANMSETMAIN:
                g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                     instruction->args.anmSetMain.scriptIdx + ANM_SCRIPT_ENEMY_START);
                break;
            case ECL_OPCODE_ANMSETSLOT:
                if (ARRAY_SIZE_SIGNED(enemy->vms) <= instruction->args.anmSetSlot.vmIdx)
                {
                    utils::DebugPrint2("error : sub anim overflow\n");
                }
                g_AnmManager->SetAndExecuteScriptIdx(&enemy->vms[instruction->args.anmSetSlot.vmIdx],
                                                     args->anmSetSlot.scriptIdx + ANM_SCRIPT_ENEMY_START);
                break;
            case ECL_OPCODE_MOVEPOSITION:
                enemy->position.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->position.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->position.z = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.z), NULL);
                enemy->ClampPos();
                break;
            case ECL_OPCODE_MOVEAXISVELOCITY:
                enemy->axisSpeed.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->axisSpeed.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->axisSpeed.z = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.z), NULL);
                enemy->flags.unk1 = 0;
                break;
            case ECL_OPCODE_MOVEVELOCITY:
                enemy->angle = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->speed = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->flags.unk1 = 1;
                break;
            case ECL_OPCODE_MOVEANGULARVELOCITY:
                enemy->angularVelocity =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->flags.unk1 = 1;
                break;
            case ECL_OPCODE_MOVEATPLAYER:
                enemy->angle = (hasSecondPlayer ? this->AngleProvokedPlayer(&enemy->position, enemy->provokedPlayer)
                                                : g_Player.AngleToPlayer(&enemy->position)) +
                               *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->speed = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->flags.unk1 = 1;
                break;
            case ECL_OPCODE_MOVESPEED:
                enemy->speed = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->flags.unk1 = 1;
                break;
            case ECL_OPCODE_MOVEACCELERATION:
                enemy->acceleration =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->flags.unk1 = 1;
                break;
            case ECL_OPCODE_BULLETFANAIMED:
            case ECL_OPCODE_BULLETFAN:
            case ECL_OPCODE_BULLETCIRCLEAIMED:
            case ECL_OPCODE_BULLETCIRCLE:
            case ECL_OPCODE_BULLETOFFSETCIRCLEAIMED:
            case ECL_OPCODE_BULLETOFFSETCIRCLE:
            case ECL_OPCODE_BULLETRANDOMANGLE:
            case ECL_OPCODE_BULLETRANDOMSPEED:
            case ECL_OPCODE_BULLETRANDOM:
                local_54 = &instruction->args.bullet;
                local_58 = &enemy->bulletProps;
                local_58->provokedPlayer = enemy->provokedPlayer;
                local_58->sprite = local_54->sprite;
                local_58->aimMode = instruction->opCode - ECL_OPCODE_BULLETFANAIMED;
                local_58->count1 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletArgs, count1), NULL);
                local_58->count1 += enemy->BulletRankAmount1(g_GameManager.rank);
                if (local_58->count1 <= 0)
                {
                    local_58->count1 = 1;
                }

                local_58->count2 = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletArgs, count2), NULL);
                local_58->count2 += enemy->BulletRankAmount2(g_GameManager.rank);
                if (local_58->count2 <= 0)
                {
                    local_58->count2 = 1;
                }
                local_58->position = enemy->position + enemy->shootOffset;
                local_58->angle1 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletArgs, angle1), NULL);
                local_58->angle1 = utils::AddNormalizeAngle(local_58->angle1, 0.0f);
                local_58->speed1 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletArgs, speed1), NULL);
                if (local_58->speed1 != 0.0f)
                {
                    local_58->speed1 += enemy->BulletRankSpeed(g_GameManager.rank);
                    if (local_58->speed1 < 0.3f)
                    {
                        local_58->speed1 = 0.3;
                    }
                }
                local_58->angle2 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletArgs, angle2), NULL);
                local_58->speed2 = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletArgs, speed2), NULL);
                local_58->speed2 += enemy->BulletRankSpeed(g_GameManager.rank) / 2.0f;
                if (local_58->speed2 < 0.3f)
                {
                    local_58->speed2 = 0.3f;
                }
                local_58->unk_4a = 0;
                local_58->flags = local_54->flags;
                local_14 = local_54->color;
                // TODO: Strict aliasing rule be like.
                local_58->spriteOffset = *EnemyEclInstr::GetVar(enemy, (EclVarId *)&local_14, NULL);
                if (enemy->flags.unk3 == 0)
                {
                    g_BulletManager.SpawnBulletPattern(local_58);
                }
                break;
            case ECL_OPCODE_BULLETEFFECTS:
                enemy->bulletProps.exInts[0] =
                    *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletEffectsArgs, ivar1), NULL);
                enemy->bulletProps.exInts[1] =
                    *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletEffectsArgs, ivar2), NULL);
                enemy->bulletProps.exInts[2] =
                    *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletEffectsArgs, ivar3), NULL);
                enemy->bulletProps.exInts[3] =
                    *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrBulletEffectsArgs, ivar4), NULL);
                enemy->bulletProps.exFloats[0] =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletEffectsArgs, fvar1), NULL);
                enemy->bulletProps.exFloats[1] =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletEffectsArgs, fvar2), NULL);
                enemy->bulletProps.exFloats[2] =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletEffectsArgs, fvar3), NULL);
                enemy->bulletProps.exFloats[3] =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrBulletEffectsArgs, fvar4), NULL);
                break;
            case ECL_OPCODE_ANMSETDEATH:
                local_5c = &instruction->args.anmSetDeath;
                enemy->deathAnm1 = local_5c->deathAnm1;
                enemy->deathAnm2 = local_5c->deathAnm2;
                enemy->deathAnm3 = local_5c->deathAnm3;
                break;
            case ECL_OPCODE_SHOOTINTERVAL:
                enemy->shootInterval = instruction->args.setInt;
                enemy->shootInterval += enemy->ShootInterval(g_GameManager.rank);
                enemy->shootIntervalTimer.SetCurrent(0);
                break;
            case ECL_OPCODE_SHOOTINTERVALDELAYED:
                enemy->shootInterval = instruction->args.setInt;
                enemy->shootInterval += enemy->ShootInterval(g_GameManager.rank);
                if (enemy->shootInterval != 0)
                {
                    enemy->shootIntervalTimer.SetCurrent(g_Rng.GetRandomU32InRange(enemy->shootInterval));
                }
                break;
            case ECL_OPCODE_SHOOTDISABLED:
                enemy->flags.unk3 = 1;
                break;
            case ECL_OPCODE_SHOOTENABLED:
                enemy->flags.unk3 = 0;
                break;
            case ECL_OPCODE_SHOOTNOW:
                enemy->bulletProps.position = enemy->position + enemy->shootOffset;
                g_BulletManager.SpawnBulletPattern(&enemy->bulletProps);
                break;
            case ECL_OPCODE_SHOOTOFFSET:
                enemy->shootOffset.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->shootOffset.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->shootOffset.z = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.z), NULL);
                break;
            case ECL_OPCODE_LASERCREATE:
            case ECL_OPCODE_LASERCREATEAIMED:
                local_64 = &instruction->args.laser;
                local_60 = &enemy->laserProps;
                local_60->provokedPlayer = enemy->provokedPlayer;
                local_60->position = enemy->position + enemy->shootOffset;
                local_60->sprite = local_64->sprite;
                local_60->spriteOffset = local_64->color;
                local_60->angle = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, angle), NULL);
                local_60->speed = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, speed), NULL);
                local_60->startOffset =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, startOffset), NULL);
                local_60->endOffset =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, endOffset), NULL);
                local_60->startLength =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, startLength), NULL);
                local_60->width =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserArgs, width), NULL);
                local_60->startTime = local_64->startTime;
                local_60->duration = local_64->duration;
                local_60->despawnDuration = local_64->despawnDuration;
                local_60->hitboxStartTime = local_64->hitboxStartTime;
                local_60->hitboxEndDelay = local_64->hitboxEndDelay;
                local_60->flags = local_64->flags;
                if (instruction->opCode == ECL_OPCODE_LASERCREATEAIMED)
                {
                    local_60->type = 0;
                }
                else
                {
                    local_60->type = 1;
                }
                enemy->lasers[enemy->laserStore] = g_BulletManager.SpawnLaserPattern(local_60);
                break;
            case ECL_OPCODE_LASERINDEX:
                enemy->laserStore = *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrAluArgs, res), NULL);
                break;
            case ECL_OPCODE_LASERROTATE:
                if (enemy->lasers[instruction->args.laserOp.laserIdx] != NULL)
                {
                    enemy->lasers[instruction->args.laserOp.laserIdx]->angle +=
                        *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserOpArgs, arg1.x), NULL);
                }
                break;
            case ECL_OPCODE_LASERROTATEFROMPLAYER:
                if (enemy->lasers[instruction->args.laserOp.laserIdx] != NULL)
                {
                    enemy->lasers[instruction->args.laserOp.laserIdx]->angle =
                        (hasSecondPlayer
                             ? this->AngleProvokedPlayer(&enemy->lasers[instruction->args.laserOp.laserIdx]->pos,
                                                         enemy->provokedPlayer)
                             : g_Player.AngleToPlayer(&enemy->lasers[instruction->args.laserOp.laserIdx]->pos)) +
                        *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserOpArgs, arg1.x), NULL);
                }
                break;
            case ECL_OPCODE_LASEROFFSET:
                if (enemy->lasers[instruction->args.laserOp.laserIdx] != NULL)
                {
                    local_98.x =
                        *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserOpArgs, arg1.x), NULL);
                    local_98.y =
                        *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserOpArgs, arg1.y), NULL);
                    local_98.z =
                        *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrLaserOpArgs, arg1.z), NULL);
                    enemy->lasers[instruction->args.laserOp.laserIdx]->pos = enemy->position + local_98;
                }
                break;
            case ECL_OPCODE_LASERTEST:
                if (enemy->lasers[instruction->args.laserOp.laserIdx] != NULL &&
                    enemy->lasers[instruction->args.laserOp.laserIdx]->inUse)
                {
                    enemy->currentContext.compareRegister = 0;
                }
                else
                {
                    enemy->currentContext.compareRegister = 1;
                }
                break;
            case ECL_OPCODE_LASERCANCEL:
                if (enemy->lasers[instruction->args.laserOp.laserIdx] != NULL &&
                    enemy->lasers[instruction->args.laserOp.laserIdx]->inUse &&
                    enemy->lasers[instruction->args.laserOp.laserIdx]->state < 2)
                {
                    enemy->lasers[instruction->args.laserOp.laserIdx]->state = 2;
                    enemy->lasers[instruction->args.laserOp.laserIdx]->timer.SetCurrent(0);
                }
                break;
            case ECL_OPCODE_LASERCLEARALL:
                for (local_68 = 0; local_68 < ARRAY_SIZE_SIGNED(enemy->lasers); local_68++)
                {
                    enemy->lasers[local_68] = NULL;
                }
                break;
            case ECL_OPCODE_BOSSSET:
                if (instruction->args.setInt >= 0)
                {
                    g_EnemyManager.bosses[instruction->args.setInt] = enemy;
                    g_Gui.bossPresent = 1;
                    g_Gui.SetBossHealthBar(1.0f);
                    enemy->flags.isBoss = 1;
                    enemy->bossId = instruction->args.setInt;
                }
                else
                {
                    g_Gui.bossPresent = 0;
                    g_EnemyManager.bosses[enemy->bossId] = NULL;
                    enemy->flags.isBoss = 0;
                }
                break;
            case ECL_OPCODE_SPELLCARDEFFECT:
                local_6c = &instruction->args.spellcardEffect;
                enemy->effectArray[enemy->effectIdx] = g_EffectManager.SpawnParticles(
                    0xd, &enemy->position, 1, (ZunColor)g_EffectsColor[local_6c->effectColorId]);
                enemy->effectArray[enemy->effectIdx]->pos2.x =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrSpellcardEffectArgs, pos.x), NULL);
                enemy->effectArray[enemy->effectIdx]->pos2.y =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrSpellcardEffectArgs, pos.y), NULL);
                enemy->effectArray[enemy->effectIdx]->pos2.z =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrSpellcardEffectArgs, pos.z), NULL);
                enemy->effectDistance =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrSpellcardEffectArgs, effectDistance), NULL);
                enemy->effectIdx++;
                break;
            case ECL_OPCODE_MOVEDIRTIMEDECELERATE:
                EnemyEclInstr::MoveDirTime(enemy, rawInstruction);
                enemy->flags.unk2 = 1;
                break;
            case ECL_OPCODE_MOVEDIRTIMEDECELERATEFAST:
                EnemyEclInstr::MoveDirTime(enemy, rawInstruction);
                enemy->flags.unk2 = 2;
                break;
            case ECL_OPCODE_MOVEDIRTIMEACCELERATE:
                EnemyEclInstr::MoveDirTime(enemy, rawInstruction);
                enemy->flags.unk2 = 3;
                break;
            case ECL_OPCODE_MOVEDIRTIMEACCELERATEFAST:
                EnemyEclInstr::MoveDirTime(enemy, rawInstruction);
                enemy->flags.unk2 = 4;
                break;
            case ECL_OPCODE_MOVEPOSITIONTIMELINEAR:
                EnemyEclInstr::MovePosTime(enemy, rawInstruction);
                enemy->flags.unk2 = 0;
                break;
            case ECL_OPCODE_MOVEPOSITIONTIMEDECELERATE:
                EnemyEclInstr::MovePosTime(enemy, rawInstruction);
                enemy->flags.unk2 = 1;
                break;
            case ECL_OPCODE_MOVEPOSITIONTIMEDECELERATEFAST:
                EnemyEclInstr::MovePosTime(enemy, rawInstruction);
                enemy->flags.unk2 = 2;
                break;
            case ECL_OPCODE_MOVEPOSITIONTIMEACCELERATE:
                EnemyEclInstr::MovePosTime(enemy, rawInstruction);
                enemy->flags.unk2 = 3;
                break;
            case ECL_OPCODE_MOVEPOSITIONTIMEACCELERATEFAST:
                EnemyEclInstr::MovePosTime(enemy, rawInstruction);
                enemy->flags.unk2 = 4;
                break;
            case ECL_OPCODE_MOVETIMEDECELERATE:
                EnemyEclInstr::MoveTime(enemy, rawInstruction);
                enemy->flags.unk2 = 1;
                break;
            case ECL_OPCODE_MOVETIMEDECELERATEFAST:
                EnemyEclInstr::MoveTime(enemy, rawInstruction);
                enemy->flags.unk2 = 2;
                break;
            case ECL_OPCODE_MOVETIMEACCELERATE:
                EnemyEclInstr::MoveTime(enemy, rawInstruction);
                enemy->flags.unk2 = 3;
                break;
            case ECL_OPCODE_MOVETIMEACCELERATEFAST:
                EnemyEclInstr::MoveTime(enemy, rawInstruction);
                enemy->flags.unk2 = 4;
                break;
            case ECL_OPCODE_MOVEBOUNDSSET:
                enemy->lowerMoveLimit.x = *EnemyEclInstr::GetVarFloat(
                    enemy, RAW_ARG_PTR(f32, EclRawInstrMoveBoundSetArgs, lowerMoveLimit.x), NULL);
                enemy->lowerMoveLimit.y = *EnemyEclInstr::GetVarFloat(
                    enemy, RAW_ARG_PTR(f32, EclRawInstrMoveBoundSetArgs, lowerMoveLimit.y), NULL);
                enemy->upperMoveLimit.x = *EnemyEclInstr::GetVarFloat(
                    enemy, RAW_ARG_PTR(f32, EclRawInstrMoveBoundSetArgs, upperMoveLimit.x), NULL);
                enemy->upperMoveLimit.y = *EnemyEclInstr::GetVarFloat(
                    enemy, RAW_ARG_PTR(f32, EclRawInstrMoveBoundSetArgs, upperMoveLimit.y), NULL);
                enemy->flags.shouldClampPos = 1;
                break;
            case ECL_OPCODE_MOVEBOUNDSDISABLE:
                enemy->flags.shouldClampPos = 0;
                break;
            case ECL_OPCODE_MOVERAND:
                local_8.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                local_8.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->angle = g_Rng.GetRandomF32InRange(local_8.y - local_8.x) + local_8.x;
                break;
            case ECL_OPCODE_MOVERANDINBOUND:
                local_8.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                local_8.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->angle = g_Rng.GetRandomF32InRange(local_8.y - local_8.x) + local_8.x;
                if (enemy->position.x < enemy->lowerMoveLimit.x + 96.0f)
                {
                    if (enemy->angle > ZUN_PI / 2.0f)
                    {
                        enemy->angle = ZUN_PI - enemy->angle;
                    }
                    else if (enemy->angle < -ZUN_PI / 2.0f)
                    {
                        enemy->angle = -ZUN_PI - enemy->angle;
                    }
                }
                if (enemy->position.x > enemy->upperMoveLimit.x - 96.0f)
                {
                    if (enemy->angle < ZUN_PI / 2.0f && enemy->angle >= 0.0f)
                    {
                        enemy->angle = ZUN_PI - enemy->angle;
                    }
                    else if (enemy->angle > -ZUN_PI / 2.0f && enemy->angle <= 0.0f)
                    {
                        enemy->angle = -ZUN_PI - enemy->angle;
                    }
                }
                if (enemy->position.y < enemy->lowerMoveLimit.y + 48.0f && enemy->angle < 0.0f)
                {
                    enemy->angle = -enemy->angle;
                }
                if (enemy->position.y > enemy->upperMoveLimit.y - 48.0f && enemy->angle > 0.0f)
                {
                    enemy->angle = -enemy->angle;
                }
                break;
            case ECL_OPCODE_ANMSETPOSES:
                enemy->anmExDefaults = instruction->args.anmSetPoses.anmExDefault;
                enemy->anmExFarLeft = instruction->args.anmSetPoses.anmExFarLeft;
                enemy->anmExFarRight = instruction->args.anmSetPoses.anmExFarRight;
                enemy->anmExLeft = instruction->args.anmSetPoses.anmExLeft;
                enemy->anmExRight = instruction->args.anmSetPoses.anmExRight;
                enemy->anmExFlags = 0xff;
                break;
            case ECL_OPCODE_ENEMYSETHITBOX:
                enemy->hitboxDimensions.x =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.x), NULL);
                enemy->hitboxDimensions.y =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.y), NULL);
                enemy->hitboxDimensions.z =
                    *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrMoveArgs, pos.z), NULL);
                break;
            case ECL_OPCODE_ENEMYFLAGCOLLISION:
                enemy->flags.unk7 = instruction->args.setInt;
                break;
            case ECL_OPCODE_ENEMYFLAGCANTAKEDAMAGE:
                enemy->flags.unk10 = instruction->args.setInt;
                break;
            case ECL_OPCODE_EFFECTSOUND:
                g_SoundPlayer.PlaySoundByIdx((SoundIdx)instruction->args.setInt, 0);
                break;
            case ECL_OPCODE_ENEMYFLAGDEATH:
                enemy->flags.unk11 = instruction->args.setInt;
                break;
            case ECL_OPCODE_DEATHCALLBACKSUB:
                enemy->deathCallbackSub = instruction->args.setInt;
                break;
            case ECL_OPCODE_ENEMYINTERRUPTSET:
                enemy->interrupts[args->setInterrupt.interruptId] = args->setInterrupt.interruptSub;
                break;
            case ECL_OPCODE_ENEMYINTERRUPT:
                enemy->runInterrupt = instruction->args.setInt;
            HANDLE_INTERRUPT:
                enemy->currentContext.currentInstr =
                    reinterpret_cast<EclRawInstr *>(const_cast<u8 *>(rawInstructionBytes + rawOffsetToNext));
                if (enemy->flags.unk14 == 0)
                {
                    memcpy(&enemy->savedContextStack[enemy->stackDepth], &enemy->currentContext,
                           sizeof(EnemyEclContext));
                }
                g_EclManager.CallEclSub(&enemy->currentContext, enemy->interrupts[enemy->runInterrupt]);
                if (enemy->stackDepth < ARRAY_SIZE_SIGNED(enemy->savedContextStack) - 1)
                {
                    enemy->stackDepth++;
                }
                enemy->runInterrupt = -1;
                continue;
            case ECL_OPCODE_ENEMYLIFESET:
                enemy->life = enemy->maxLife = instruction->args.setInt;
                break;
            case ECL_OPCODE_SPELLCARDSTART:
                g_Gui.ShowSpellcard(instruction->args.spellcardStart.spellcardSprite,
                                    RAW_ARG_PTR(char, EclRawInstrSpellcardStartArgs, spellcardName));
                g_EnemyManager.spellcardInfo.isCapturing = 1;
                g_EnemyManager.spellcardInfo.isActive = 1;
                g_EnemyManager.spellcardInfo.idx = instruction->args.spellcardStart.spellcardId;
                g_EnemyManager.spellcardInfo.captureScore = g_SpellcardScore[g_EnemyManager.spellcardInfo.idx];
                THPrac::TH06::THPracLockTimerReset();
                THPrac::TH06::THPracSpellAttempt();
                g_BulletManager.TurnAllBulletsIntoPoints();
                g_Stage.spellcardState = RUNNING;
                g_Stage.ticksSinceSpellcardStarted = 0;
                enemy->bulletRankSpeedLow = -0.5f;
                enemy->bulletRankSpeedHigh = 0.5f;
                enemy->bulletRankAmount1Low = 0;
                enemy->bulletRankAmount1High = 0;
                enemy->bulletRankAmount2Low = 0;
                enemy->bulletRankAmount2High = 0;
                local_70 = &g_GameManager.catk[g_EnemyManager.spellcardInfo.idx];
                csum = 0;
                if (!g_GameManager.isInReplay)
                {
                    strcpy(local_70->name, RAW_ARG_PTR(char, EclRawInstrSpellcardStartArgs, spellcardName));
                    local_74 = strlen(local_70->name);
                    while (0 < local_74)
                    {
                        csum += local_70->name[--local_74];
                    }
                    if (local_70->nameCsum != (u8)csum)
                    {
                        local_70->numSuccess = 0;
                        local_70->numAttempts = 0;
                        local_70->nameCsum = csum;
                    }
                    local_70->captureScore = g_EnemyManager.spellcardInfo.captureScore;
                    if (local_70->numAttempts < 9999)
                    {
                        local_70->numAttempts++;
                    }
                }
                break;
            case ECL_OPCODE_SPELLCARDEND:
                if (g_EnemyManager.spellcardInfo.isActive)
                {
                    g_Gui.EndEnemySpellcard();
                    if (g_EnemyManager.spellcardInfo.isActive == 1)
                    {
                        scoreIncrease = g_BulletManager.DespawnBullets(12800, 1);
                        if (g_EnemyManager.spellcardInfo.isCapturing)
                        {
                            local_80 = &g_GameManager.catk[g_EnemyManager.spellcardInfo.idx];
                            local_88 = g_EnemyManager.spellcardInfo.captureScore >= 500000
                                           ? 500000 / 10
                                           : g_EnemyManager.spellcardInfo.captureScore / 10;
                            scoreIncrease =
                                g_EnemyManager.spellcardInfo.captureScore +
                                g_EnemyManager.spellcardInfo.captureScore * g_Gui.SpellcardSecondsRemaining() / 10;
                            g_Gui.ShowSpellcardBonus(scoreIncrease);
                            g_GameManager.score += scoreIncrease;
                            if (!g_GameManager.isInReplay)
                            {
                                local_80->numSuccess++;
                                // What. the. fuck?
                                // memmove(&local_80->nameCsum, &local_80->characterShotType, 4);
                                for (local_84 = 4; 0 < local_84; local_84 = local_84 + -1)
                                {
                                    ((u8 *)&local_80->nameCsum)[local_84 + 1] = ((u8 *)&local_80->nameCsum)[local_84];
                                }
                                local_80->characterShotType = g_GameManager.CharacterShotType();
                            }
                            g_GameManager.spellcardsCaptured++;
                            THPrac::TH06::THPracSpellCapture();
                        }
                    }
                    g_EnemyManager.spellcardInfo.isActive = 0;
                }
                g_Stage.spellcardState = NOT_RUNNING;
                break;
            case ECL_OPCODE_BOSSTIMERSET:
                enemy->bossTimer.SetCurrent(instruction->args.setInt);
                break;
            case ECL_OPCODE_LIFECALLBACKTHRESHOLD:
                enemy->lifeCallbackThreshold = instruction->args.setInt;
                break;
            case ECL_OPCODE_LIFECALLBACKSUB:
                enemy->lifeCallbackSub = instruction->args.setInt;
                break;
            case ECL_OPCODE_TIMERCALLBACKTHRESHOLD:
                enemy->timerCallbackThreshold = instruction->args.setInt;
                enemy->bossTimer.SetCurrent(0);
                break;
            case ECL_OPCODE_TIMERCALLBACKSUB:
                enemy->timerCallbackSub = instruction->args.setInt;
                break;
            case ECL_OPCODE_ENEMYFLAGINTERACTABLE:
                enemy->flags.unk6 = instruction->args.setInt;
                break;
            case ECL_OPCODE_EFFECTPARTICLE:
                g_EffectManager.SpawnParticles(instruction->args.effectParticle.effectId, &enemy->position,
                                               instruction->args.effectParticle.numParticles,
                                               instruction->args.effectParticle.particleColor);
                break;
            case ECL_OPCODE_DROPITEMS:
                for (local_8c = 0; local_8c < instruction->args.setInt; local_8c++)
                {
                    local_98 = enemy->position;

                    local_98[0] += g_Rng.GetRandomF32InRange(144.0f) - 72.0f;
                    local_98[1] += g_Rng.GetRandomF32InRange(144.0f) - 72.0f;
                    if (hasSecondPlayer ? (g_GameManager.currentPower < 128 || g_GameManager.currentPower2 < 128)
                                        : (g_GameManager.currentPower < 128))
                    {
                        g_ItemManager.SpawnItem(&local_98, local_8c == 0 ? ITEM_POWER_BIG : ITEM_POWER_SMALL, 0);
                    }
                    else
                    {
                        g_ItemManager.SpawnItem(&local_98, ITEM_POINT, 0);
                    }
                }
                break;
            case ECL_OPCODE_ANMFLAGROTATION:
                enemy->flags.unk13 = instruction->args.setInt;
                break;
            case ECL_OPCODE_EXINSCALL:
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d exins=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, instruction->args.setInt);
                }
#endif
                g_EclExInsn[instruction->args.setInt](enemy, rawInstruction);
                break;
            case ECL_OPCODE_EXINSREPEAT:
                if (instruction->args.setInt >= 0)
                {
                    enemy->currentContext.funcSetFunc = g_EclExInsn[instruction->args.setInt];
                }
                else
                {
                    enemy->currentContext.funcSetFunc = NULL;
                }
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d exins_repeat=%d active=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, instruction->args.setInt,
                            instruction->args.setInt >= 0 ? 1 : 0);
                }
#endif
                break;
            case ECL_OPCODE_TIMESET:
                enemy->currentContext.time.IncrementInline(
                    *EnemyEclInstr::GetVar(enemy, RAW_ARG_PTR(EclVarId, EclRawInstrTimeSetArgs, timeToSet), NULL));
                break;
            case ECL_OPCODE_DROPITEMID:
                g_ItemManager.SpawnItem(&enemy->position, instruction->args.dropItem.itemId, 0);
                break;
            case ECL_OPCODE_STDUNPAUSE:
                g_Stage.unpauseFlag = 1;
                break;
            case ECL_OPCODE_BOSSSETLIFECOUNT:
                g_Gui.eclSetLives = instruction->args.GetBossLifeCount();
                g_GameManager.counat += 1800;
                break;
            case ECL_OPCODE_ENEMYCREATE:
                local_b0.subId = instruction->args.enemyCreate.subId;
                local_b0.pos.x = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrEnemyCreateArgs, pos.x), NULL);
                local_b0.pos.y = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrEnemyCreateArgs, pos.y), NULL);
                local_b0.pos.z = *EnemyEclInstr::GetVarFloat(enemy, RAW_ARG_PTR(f32, EclRawInstrEnemyCreateArgs, pos.z), NULL);
                local_b0.life = instruction->args.enemyCreate.life;
                local_b0.itemDrop = instruction->args.enemyCreate.itemDrop;
                local_b0.score = instruction->args.enemyCreate.score;
#if defined(DEBUG) || defined(TH06_ECL_TRACE)
                if (enemy->flags.isBoss && g_EnemyManager.spellcardInfo.isActive)
                {
                    SDL_Log("[spell/ecl/detail] boss=%d spell=%d sub=%u time=%d op=%d spawn_sub=%d pos=(%f,%f,%f) life=%d item=%d score=%d",
                            enemy->bossId, g_EnemyManager.spellcardInfo.idx, enemy->currentContext.subId,
                            enemy->currentContext.time.current, instruction->opCode, local_b0.subId, local_b0.pos.x,
                            local_b0.pos.y, local_b0.pos.z, local_b0.life, local_b0.itemDrop, local_b0.score);
                }
#endif
                g_EnemyManager.SpawnEnemy(local_b0.subId, local_b0.pos.AsD3dXVec(), local_b0.life, local_b0.itemDrop,
                                          local_b0.score);
                break;
            case ECL_OPCODE_ENEMYKILLALL:
                for (local_b4 = &g_EnemyManager.enemies[0], local_b8 = 0;
                     local_b8 < ARRAY_SIZE_SIGNED(g_EnemyManager.enemies) - 1; local_b8++, local_b4++)
                {
                    if (!local_b4->flags.unk5)
                    {
                        continue;
                    }
                    if (local_b4->flags.isBoss)
                    {
                        continue;
                    }

                    local_b4->life = 0;
                    if (local_b4->flags.unk6 == 0 && 0 <= local_b4->deathCallbackSub)
                    {
                        g_EclManager.CallEclSub(&local_b4->currentContext, local_b4->deathCallbackSub);
                        local_b4->deathCallbackSub = -1;
                    }
                }
                break;
            case ECL_OPCODE_ANMINTERRUPTMAIN:
                enemy->primaryVm.pendingInterrupt = instruction->args.setInt;
                break;
            case ECL_OPCODE_ANMINTERRUPTSLOT:
                enemy->vms[args->anmInterruptSlot.vmId].pendingInterrupt = args->anmInterruptSlot.interruptId;
                break;
            case ECL_OPCODE_BULLETCANCEL:
                g_BulletManager.TurnAllBulletsIntoPoints();
                break;
            case ECL_OPCODE_BULLETSOUND:
                if (instruction->args.bulletSound.bulletSfx >= 0)
                {
                    enemy->bulletProps.sfx = instruction->args.bulletSound.bulletSfx;
                    enemy->bulletProps.flags |= 0x200;
                }
                else
                {
                    enemy->bulletProps.flags &= 0xfffffdff;
                }
                break;
            case ECL_OPCODE_ENEMYFLAGDISABLECALLSTACK:
                enemy->flags.unk14 = instruction->args.setInt;
                break;
            case ECL_OPCODE_BULLETRANKINFLUENCE:
                enemy->bulletRankSpeedLow = args->bulletRankInfluence.bulletRankSpeedLow;
                enemy->bulletRankSpeedHigh = args->bulletRankInfluence.bulletRankSpeedHigh;
                enemy->bulletRankAmount1Low = args->bulletRankInfluence.bulletRankAmount1Low;
                enemy->bulletRankAmount1High = args->bulletRankInfluence.bulletRankAmount1High;
                enemy->bulletRankAmount2Low = args->bulletRankInfluence.bulletRankAmount2Low;
                enemy->bulletRankAmount2High = args->bulletRankInfluence.bulletRankAmount2High;
                break;
            case ECL_OPCODE_ENEMYFLAGINVISIBLE:
                enemy->flags.unk15 = instruction->args.setInt;
                break;
            case ECL_OPCODE_BOSSTIMERCLEAR:
                enemy->timerCallbackSub = enemy->deathCallbackSub;
                enemy->bossTimer.SetCurrent(0);
                break;
            case ECL_OPCODE_SPELLCARDFLAGTIMEOUT:
                enemy->flags.unk16 = instruction->args.setInt;
                break;
            }
        NEXT_INSN:
            rawInstruction = reinterpret_cast<EclRawInstr *>(const_cast<u8 *>(rawInstructionBytes + rawOffsetToNext));
            goto YOLO;
        }
        else
        {
            switch (enemy->flags.unk1)
            {
            case 1:
                enemy->angle = utils::AddNormalizeAngle(enemy->angle, g_Supervisor.effectiveFramerateMultiplier *
                                                                          enemy->angularVelocity);
                enemy->speed = g_Supervisor.effectiveFramerateMultiplier * enemy->acceleration + enemy->speed;
                sincosmul(&enemy->axisSpeed, enemy->angle, enemy->speed);
                enemy->axisSpeed.z = 0.0;
                break;
            case 2:
                enemy->moveInterpTimer.Decrement(1);
                local_bc = enemy->moveInterpTimer.AsFramesFloat() / enemy->moveInterpStartTime;
                if (local_bc >= 1.0f)
                {
                    local_bc = 1.0f;
                }
                switch (enemy->flags.unk2)
                {
                case 0:
                    local_bc = 1.0f - local_bc;
                    break;
                case 1:
                    local_bc = 1.0f - local_bc * local_bc;
                    break;
                case 2:
                    local_bc = 1.0f - local_bc * local_bc * local_bc * local_bc;
                    break;
                case 3:
                    local_bc = 1.0f - local_bc;
                    local_bc *= local_bc;
                    break;
                case 4:
                    local_bc = 1.0f - local_bc;
                    local_bc = local_bc * local_bc * local_bc * local_bc;
                }
                enemy->axisSpeed = local_bc * enemy->moveInterp + enemy->moveInterpStartPos - enemy->position;
                enemy->angle = atan2f(enemy->axisSpeed.y, enemy->axisSpeed.x);
                if ((ZunBool)(enemy->moveInterpTimer.current <= 0))
                {
                    enemy->flags.unk1 = 0;
                    enemy->position = enemy->moveInterpStartPos + enemy->moveInterp;
                    enemy->axisSpeed = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
                }
                break;
            }
            if (0 < enemy->life)
            {
                if (0 < enemy->shootInterval)
                {
                    enemy->shootIntervalTimer.Tick();
                    if ((ZunBool)(enemy->shootIntervalTimer.current >= enemy->shootInterval))
                    {
                        enemy->bulletProps.position = enemy->position + enemy->shootOffset;
                        g_BulletManager.SpawnBulletPattern(&enemy->bulletProps);
                        enemy->shootIntervalTimer.InitializeForPopup();
                    }
                }
                if (0 <= enemy->anmExLeft)
                {
                    local_c0 = 0;
                    if (enemy->axisSpeed.x < 0.0f)
                    {
                        local_c0 = 1;
                    }
                    else if (enemy->axisSpeed.x > 0.0f)
                    {
                        local_c0 = 2;
                    }
                    if (enemy->anmExFlags != local_c0)
                    {
                        switch (local_c0)
                        {
                        case 0:
                            if (enemy->anmExFlags == 0xff)
                            {
                                g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                                     enemy->anmExDefaults + ANM_OFFSET_ENEMY);
                            }
                            else if (enemy->anmExFlags == 1)
                            {
                                g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                                     enemy->anmExFarLeft + ANM_OFFSET_ENEMY);
                            }
                            else
                            {
                                g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                                     enemy->anmExFarRight + ANM_OFFSET_ENEMY);
                            }
                            break;
                        case 1:
                            g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                                 enemy->anmExLeft + ANM_OFFSET_ENEMY);
                            break;
                        case 2:
                            g_AnmManager->SetAndExecuteScriptIdx(&enemy->primaryVm,
                                                                 enemy->anmExRight + ANM_OFFSET_ENEMY);
                            break;
                        }
                        enemy->anmExFlags = local_c0;
                    }
                }
                if (enemy->currentContext.funcSetFunc != NULL)
                {
                    enemy->currentContext.funcSetFunc(enemy, NULL);
                }
            }
            enemy->currentContext.currentInstr = rawInstruction;
            enemy->currentContext.time.Tick();
            return ZUN_SUCCESS;
        }
    }
#undef RAW_ARG_PTR
}
}; // namespace th06
