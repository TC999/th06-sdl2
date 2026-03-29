#include "GameplayStatePortable.hpp"

#include <cstring>
#include <memory>

namespace th06::DGS
{
namespace
{
struct BufferWriter
{
    std::vector<u8> bytes;

    void WriteU8(u8 value)
    {
        bytes.push_back(value);
    }

    void WriteU16(u16 value)
    {
        bytes.push_back((u8)(value & 0xff));
        bytes.push_back((u8)((value >> 8) & 0xff));
    }

    void WriteU32(u32 value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back((u8)((value >> shift) & 0xff));
        }
    }

    void WriteU64(u64 value)
    {
        for (int shift = 0; shift < 64; shift += 8)
        {
            bytes.push_back((u8)((value >> shift) & 0xff));
        }
    }

    void WriteI16(i16 value)
    {
        WriteU16((u16)value);
    }

    void WriteI32(i32 value)
    {
        WriteU32((u32)value);
    }

    void WriteF32(f32 value)
    {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteU32(bits);
    }

    void WriteF64(double value)
    {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteU64(bits);
    }

    void WriteTimer(const ZunTimer &timer)
    {
        WriteI32(timer.previous);
        WriteF32(timer.subFrame);
        WriteI32(timer.current);
    }

    void WriteVec2(const D3DXVECTOR2 &vec)
    {
        WriteF32(vec.x);
        WriteF32(vec.y);
    }

    void WriteVec3(const D3DXVECTOR3 &vec)
    {
        WriteF32(vec.x);
        WriteF32(vec.y);
        WriteF32(vec.z);
    }

    void WriteString(const std::string &value)
    {
        WriteU32((u32)value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

struct BufferReader
{
    const std::vector<u8> &bytes;
    size_t cursor = 0;

    bool CanRead(size_t count) const
    {
        return cursor + count <= bytes.size();
    }

    bool ReadU8(u8 &value)
    {
        if (!CanRead(1))
        {
            return false;
        }
        value = bytes[cursor++];
        return true;
    }

    bool ReadU16(u16 &value)
    {
        if (!CanRead(2))
        {
            return false;
        }
        value = (u16)(bytes[cursor] | (bytes[cursor + 1] << 8));
        cursor += 2;
        return true;
    }

    bool ReadU32(u32 &value)
    {
        if (!CanRead(4))
        {
            return false;
        }
        value = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            value |= (u32)bytes[cursor++] << shift;
        }
        return true;
    }

    bool ReadU64(u64 &value)
    {
        if (!CanRead(8))
        {
            return false;
        }
        value = 0;
        for (int shift = 0; shift < 64; shift += 8)
        {
            value |= (u64)bytes[cursor++] << shift;
        }
        return true;
    }

    bool ReadI16(i16 &value)
    {
        u16 raw = 0;
        if (!ReadU16(raw))
        {
            return false;
        }
        value = (i16)raw;
        return true;
    }

    bool ReadI32(i32 &value)
    {
        u32 raw = 0;
        if (!ReadU32(raw))
        {
            return false;
        }
        value = (i32)raw;
        return true;
    }

    bool ReadF32(f32 &value)
    {
        u32 bits = 0;
        if (!ReadU32(bits))
        {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool ReadF64(double &value)
    {
        u64 bits = 0;
        if (!ReadU64(bits))
        {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool ReadTimer(ZunTimer &timer)
    {
        return ReadI32(timer.previous) && ReadF32(timer.subFrame) && ReadI32(timer.current);
    }

    bool ReadVec2(D3DXVECTOR2 &vec)
    {
        return ReadF32(vec.x) && ReadF32(vec.y);
    }

    bool ReadVec3(D3DXVECTOR3 &vec)
    {
        return ReadF32(vec.x) && ReadF32(vec.y) && ReadF32(vec.z);
    }

    bool ReadString(std::string &value)
    {
        u32 size = 0;
        if (!ReadU32(size) || !CanRead(size))
        {
            return false;
        }

        value.assign(reinterpret_cast<const char *>(&bytes[cursor]), size);
        cursor += size;
        return true;
    }
};

void WriteSectionHeader(BufferWriter &writer, PortableGameplaySectionId id, const std::vector<u8> &payload)
{
    writer.WriteU16((u16)id);
    writer.WriteU16(1);
    writer.WriteU32((u32)payload.size());
    writer.bytes.insert(writer.bytes.end(), payload.begin(), payload.end());
}

void WriteAnmVmRefs(BufferWriter &writer, const DgsAnmVmRefs &refs)
{
    writer.WriteI32(refs.scriptSlot);
    writer.WriteI32(refs.beginningOfScript.value);
    writer.WriteI32(refs.currentInstruction.value);
    writer.WriteI32(refs.spriteIndex.value);
}

bool ReadAnmVmRefs(BufferReader &reader, DgsAnmVmRefs &refs)
{
    return reader.ReadI32(refs.scriptSlot) && reader.ReadI32(refs.beginningOfScript.value) &&
           reader.ReadI32(refs.currentInstruction.value) && reader.ReadI32(refs.spriteIndex.value);
}

void WriteEnemyContextRefs(BufferWriter &writer, const DgsEnemyContextRefs &refs)
{
    writer.WriteI32(refs.currentInstruction.value);
    writer.WriteU8(refs.hasFuncSetFunc ? 1 : 0);
    writer.WriteU8(refs.unresolvedFuncSetFunc ? 1 : 0);
}

bool ReadEnemyContextRefs(BufferReader &reader, DgsEnemyContextRefs &refs)
{
    u8 hasFunc = 0;
    u8 unresolved = 0;
    if (!reader.ReadI32(refs.currentInstruction.value) || !reader.ReadU8(hasFunc) || !reader.ReadU8(unresolved))
    {
        return false;
    }
    refs.hasFuncSetFunc = hasFunc != 0;
    refs.unresolvedFuncSetFunc = unresolved != 0;
    return true;
}

void WriteRuntimeEffectState(BufferWriter &writer, const ScreenEffect::RuntimeEffectState &effect)
{
    writer.WriteI32(effect.usedEffect);
    writer.WriteI32(effect.fadeAlpha);
    writer.WriteI32(effect.effectLength);
    writer.WriteI32(effect.genericParam);
    writer.WriteI32(effect.shakinessParam);
    writer.WriteI32(effect.unusedParam);
    writer.WriteTimer(effect.timer);
}

bool ReadRuntimeEffectState(BufferReader &reader, ScreenEffect::RuntimeEffectState &effect)
{
    return reader.ReadI32(effect.usedEffect) && reader.ReadI32(effect.fadeAlpha) && reader.ReadI32(effect.effectLength) &&
           reader.ReadI32(effect.genericParam) && reader.ReadI32(effect.shakinessParam) &&
           reader.ReadI32(effect.unusedParam) && reader.ReadTimer(effect.timer);
}

void WriteMatrix(BufferWriter &writer, const D3DXMATRIX &matrix)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            writer.WriteF32(matrix.m[row][col]);
        }
    }
}

bool ReadMatrix(BufferReader &reader, D3DXMATRIX &matrix)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            if (!reader.ReadF32(matrix.m[row][col]))
            {
                return false;
            }
        }
    }
    return true;
}

void WritePortableEnemyFlags(BufferWriter &writer, const PortableEnemyFlagsState &flags)
{
    writer.WriteU8(flags.unk1);
    writer.WriteU8(flags.unk2);
    writer.WriteU8(flags.unk3);
    writer.WriteU8(flags.unk4);
    writer.WriteU8(flags.unk5);
    writer.WriteU8(flags.unk6);
    writer.WriteU8(flags.unk7);
    writer.WriteU8(flags.unk8);
    writer.WriteU8(flags.isBoss);
    writer.WriteU8(flags.unk10);
    writer.WriteU8(flags.unk11);
    writer.WriteU8(flags.shouldClampPos);
    writer.WriteU8(flags.unk13);
    writer.WriteU8(flags.unk14);
    writer.WriteU8(flags.unk15);
    writer.WriteU8(flags.unk16);
}

bool ReadPortableEnemyFlags(BufferReader &reader, PortableEnemyFlagsState &flags)
{
    return reader.ReadU8(flags.unk1) && reader.ReadU8(flags.unk2) && reader.ReadU8(flags.unk3) &&
           reader.ReadU8(flags.unk4) && reader.ReadU8(flags.unk5) && reader.ReadU8(flags.unk6) &&
           reader.ReadU8(flags.unk7) && reader.ReadU8(flags.unk8) && reader.ReadU8(flags.isBoss) &&
           reader.ReadU8(flags.unk10) && reader.ReadU8(flags.unk11) && reader.ReadU8(flags.shouldClampPos) &&
           reader.ReadU8(flags.unk13) && reader.ReadU8(flags.unk14) && reader.ReadU8(flags.unk15) &&
           reader.ReadU8(flags.unk16);
}

void WritePortableAnmVmState(BufferWriter &writer, const PortableAnmVmState &state)
{
    writer.WriteVec3(state.rotation);
    writer.WriteVec3(state.angleVel);
    writer.WriteF32(state.scaleY);
    writer.WriteF32(state.scaleX);
    writer.WriteF32(state.scaleInterpFinalY);
    writer.WriteF32(state.scaleInterpFinalX);
    writer.WriteVec2(state.uvScrollPos);
    writer.WriteTimer(state.currentTimeInScript);
    WriteMatrix(writer, state.matrix);
    writer.WriteU32(state.color);
    writer.WriteU16(state.flags);
    writer.WriteI16(state.alphaInterpEndTime);
    writer.WriteI16(state.scaleInterpEndTime);
    writer.WriteI16(state.autoRotate);
    writer.WriteI16(state.pendingInterrupt);
    writer.WriteI16(state.posInterpEndTime);
    writer.WriteVec3(state.pos);
    writer.WriteF32(state.scaleInterpInitialY);
    writer.WriteF32(state.scaleInterpInitialX);
    writer.WriteTimer(state.scaleInterpTime);
    writer.WriteI16(state.activeSpriteIndex);
    writer.WriteI16(state.baseSpriteIndex);
    writer.WriteI16(state.anmFileIndex);
    writer.WriteU32(state.alphaInterpInitial);
    writer.WriteU32(state.alphaInterpFinal);
    writer.WriteVec3(state.posInterpInitial);
    writer.WriteVec3(state.posInterpFinal);
    writer.WriteVec3(state.posOffset);
    writer.WriteTimer(state.posInterpTime);
    writer.WriteI32(state.timeOfLastSpriteSet);
    writer.WriteTimer(state.alphaInterpTime);
    writer.WriteU8(state.fontWidth);
    writer.WriteU8(state.fontHeight);
    WriteAnmVmRefs(writer, state.refs);
}

bool ReadPortableAnmVmState(BufferReader &reader, PortableAnmVmState &state)
{
    return reader.ReadVec3(state.rotation) && reader.ReadVec3(state.angleVel) && reader.ReadF32(state.scaleY) &&
           reader.ReadF32(state.scaleX) && reader.ReadF32(state.scaleInterpFinalY) &&
           reader.ReadF32(state.scaleInterpFinalX) && reader.ReadVec2(state.uvScrollPos) &&
           reader.ReadTimer(state.currentTimeInScript) && ReadMatrix(reader, state.matrix) &&
           reader.ReadU32(state.color) && reader.ReadU16(state.flags) &&
           reader.ReadI16(state.alphaInterpEndTime) && reader.ReadI16(state.scaleInterpEndTime) &&
           reader.ReadI16(state.autoRotate) && reader.ReadI16(state.pendingInterrupt) &&
           reader.ReadI16(state.posInterpEndTime) && reader.ReadVec3(state.pos) &&
           reader.ReadF32(state.scaleInterpInitialY) && reader.ReadF32(state.scaleInterpInitialX) &&
           reader.ReadTimer(state.scaleInterpTime) && reader.ReadI16(state.activeSpriteIndex) &&
           reader.ReadI16(state.baseSpriteIndex) && reader.ReadI16(state.anmFileIndex) &&
           reader.ReadU32(state.alphaInterpInitial) && reader.ReadU32(state.alphaInterpFinal) &&
           reader.ReadVec3(state.posInterpInitial) && reader.ReadVec3(state.posInterpFinal) &&
           reader.ReadVec3(state.posOffset) && reader.ReadTimer(state.posInterpTime) &&
           reader.ReadI32(state.timeOfLastSpriteSet) && reader.ReadTimer(state.alphaInterpTime) &&
           reader.ReadU8(state.fontWidth) && reader.ReadU8(state.fontHeight) && ReadAnmVmRefs(reader, state.refs);
}

void WritePortableEnemyContext(BufferWriter &writer, const PortableEnemyEclContextState &state)
{
    writer.WriteI32(state.scriptOffset);
    writer.WriteTimer(state.time);
    writer.WriteU16((u16)state.funcToken);
    writer.WriteU8(state.hasFuncToken ? 1 : 0);
    writer.WriteU8(state.hasUnknownFuncToken ? 1 : 0);
    writer.WriteI32(state.var0);
    writer.WriteI32(state.var1);
    writer.WriteI32(state.var2);
    writer.WriteI32(state.var3);
    writer.WriteF32(state.float0);
    writer.WriteF32(state.float1);
    writer.WriteF32(state.float2);
    writer.WriteF32(state.float3);
    writer.WriteI32(state.var4);
    writer.WriteI32(state.var5);
    writer.WriteI32(state.var6);
    writer.WriteI32(state.var7);
    writer.WriteI32(state.compareRegister);
    writer.WriteU16(state.subId);
}

bool ReadPortableEnemyContext(BufferReader &reader, PortableEnemyEclContextState &state)
{
    u16 token = 0;
    u8 hasFunc = 0;
    u8 hasUnknown = 0;
    if (!reader.ReadI32(state.scriptOffset) || !reader.ReadTimer(state.time) || !reader.ReadU16(token) ||
        !reader.ReadU8(hasFunc) || !reader.ReadU8(hasUnknown))
    {
        return false;
    }
    state.funcToken = (PortableEnemyFuncToken)token;
    state.hasFuncToken = hasFunc != 0;
    state.hasUnknownFuncToken = hasUnknown != 0;
    return reader.ReadI32(state.var0) && reader.ReadI32(state.var1) && reader.ReadI32(state.var2) &&
           reader.ReadI32(state.var3) && reader.ReadF32(state.float0) && reader.ReadF32(state.float1) &&
           reader.ReadF32(state.float2) && reader.ReadF32(state.float3) && reader.ReadI32(state.var4) &&
           reader.ReadI32(state.var5) && reader.ReadI32(state.var6) && reader.ReadI32(state.var7) &&
           reader.ReadI32(state.compareRegister) && reader.ReadU16(state.subId);
}

void WritePortableEnemyBulletShooter(BufferWriter &writer, const PortableEnemyBulletShooterState &state)
{
    writer.WriteI16(state.sprite);
    writer.WriteI16(state.spriteOffset);
    writer.WriteVec3(state.position);
    writer.WriteF32(state.angle1);
    writer.WriteF32(state.angle2);
    writer.WriteF32(state.speed1);
    writer.WriteF32(state.speed2);
    for (f32 value : state.exFloats)
        writer.WriteF32(value);
    for (i32 value : state.exInts)
        writer.WriteI32(value);
    writer.WriteI32(state.unk_40);
    writer.WriteI16(state.count1);
    writer.WriteI16(state.count2);
    writer.WriteU16(state.aimMode);
    writer.WriteU16(state.unk_4a);
    writer.WriteU32(state.flags);
    writer.WriteU8(state.provokedPlayer);
    writer.WriteI32((i32)state.sfx);
}

bool ReadPortableEnemyBulletShooter(BufferReader &reader, PortableEnemyBulletShooterState &state)
{
    i32 sound = 0;
    if (!reader.ReadI16(state.sprite) || !reader.ReadI16(state.spriteOffset) || !reader.ReadVec3(state.position) ||
        !reader.ReadF32(state.angle1) || !reader.ReadF32(state.angle2) || !reader.ReadF32(state.speed1) ||
        !reader.ReadF32(state.speed2))
    {
        return false;
    }
    for (f32 &value : state.exFloats)
    {
        if (!reader.ReadF32(value))
            return false;
    }
    for (i32 &value : state.exInts)
    {
        if (!reader.ReadI32(value))
            return false;
    }
    if (!reader.ReadI32(state.unk_40) || !reader.ReadI16(state.count1) || !reader.ReadI16(state.count2) ||
        !reader.ReadU16(state.aimMode) || !reader.ReadU16(state.unk_4a) || !reader.ReadU32(state.flags) ||
        !reader.ReadU8(state.provokedPlayer) || !reader.ReadI32(sound))
    {
        return false;
    }
    state.sfx = (SoundIdx)sound;
    return true;
}

void WritePortableEnemyLaserShooter(BufferWriter &writer, const PortableEnemyLaserShooterState &state)
{
    writer.WriteI16(state.sprite);
    writer.WriteI16(state.spriteOffset);
    writer.WriteVec3(state.position);
    writer.WriteF32(state.angle);
    writer.WriteU32(state.unk_14);
    writer.WriteF32(state.speed);
    writer.WriteU32(state.unk_1c);
    writer.WriteF32(state.startOffset);
    writer.WriteF32(state.endOffset);
    writer.WriteF32(state.startLength);
    writer.WriteF32(state.width);
    writer.WriteI32(state.startTime);
    writer.WriteI32(state.duration);
    writer.WriteI32(state.despawnDuration);
    writer.WriteI32(state.hitboxStartTime);
    writer.WriteI32(state.hitboxEndDelay);
    writer.WriteU32(state.unk_44);
    writer.WriteU16(state.type);
    writer.WriteU32(state.flags);
    writer.WriteU32(state.unk_50);
    writer.WriteU8(state.provokedPlayer);
}

bool ReadPortableEnemyLaserShooter(BufferReader &reader, PortableEnemyLaserShooterState &state)
{
    return reader.ReadI16(state.sprite) && reader.ReadI16(state.spriteOffset) && reader.ReadVec3(state.position) &&
           reader.ReadF32(state.angle) && reader.ReadU32(state.unk_14) && reader.ReadF32(state.speed) &&
           reader.ReadU32(state.unk_1c) && reader.ReadF32(state.startOffset) && reader.ReadF32(state.endOffset) &&
           reader.ReadF32(state.startLength) && reader.ReadF32(state.width) && reader.ReadI32(state.startTime) &&
           reader.ReadI32(state.duration) && reader.ReadI32(state.despawnDuration) &&
           reader.ReadI32(state.hitboxStartTime) && reader.ReadI32(state.hitboxEndDelay) &&
           reader.ReadU32(state.unk_44) && reader.ReadU16(state.type) && reader.ReadU32(state.flags) &&
           reader.ReadU32(state.unk_50) && reader.ReadU8(state.provokedPlayer);
}

void WritePortableRunningSpellcard(BufferWriter &writer, const PortableRunningSpellcardState &state)
{
    writer.WriteU8(state.isCapturing ? 1 : 0);
    writer.WriteU8(state.isActive ? 1 : 0);
    writer.WriteI32(state.captureScore);
    writer.WriteU32(state.idx);
    writer.WriteU8(state.usedBomb ? 1 : 0);
}

bool ReadPortableRunningSpellcard(BufferReader &reader, PortableRunningSpellcardState &state)
{
    u8 isCapturing = 0;
    u8 isActive = 0;
    u8 usedBomb = 0;
    if (!reader.ReadU8(isCapturing) || !reader.ReadU8(isActive) || !reader.ReadI32(state.captureScore) ||
        !reader.ReadU32(state.idx) || !reader.ReadU8(usedBomb))
    {
        return false;
    }
    state.isCapturing = isCapturing != 0;
    state.isActive = isActive != 0;
    state.usedBomb = usedBomb != 0;
    return true;
}

void WritePortablePlayerBullet(BufferWriter &writer, const PortablePlayerBulletState &state)
{
    WritePortableAnmVmState(writer, state.sprite);
    writer.WriteVec3(state.position);
    writer.WriteVec3(state.size);
    writer.WriteVec2(state.velocity);
    writer.WriteF32(state.sidewaysMotion);
    writer.WriteVec3(state.unk_134);
    writer.WriteTimer(state.timer);
    writer.WriteI16(state.damage);
    writer.WriteI16(state.bulletState);
    writer.WriteI16(state.bulletType);
    writer.WriteI16(state.unk_152);
    writer.WriteI16(state.spawnPositionIdx);
}

bool ReadPortablePlayerBullet(BufferReader &reader, PortablePlayerBulletState &state)
{
    return ReadPortableAnmVmState(reader, state.sprite) && reader.ReadVec3(state.position) &&
           reader.ReadVec3(state.size) && reader.ReadVec2(state.velocity) && reader.ReadF32(state.sidewaysMotion) &&
           reader.ReadVec3(state.unk_134) && reader.ReadTimer(state.timer) && reader.ReadI16(state.damage) &&
           reader.ReadI16(state.bulletState) && reader.ReadI16(state.bulletType) && reader.ReadI16(state.unk_152) &&
           reader.ReadI16(state.spawnPositionIdx);
}

void WritePortablePlayerBomb(BufferWriter &writer, const PortablePlayerBombState &state)
{
    writer.WriteU32(state.isInUse);
    writer.WriteI32(state.duration);
    writer.WriteTimer(state.timer);
    writer.WriteU16((u16)state.calcToken);
    writer.WriteU16((u16)state.drawToken);
    writer.WriteU8(state.hasUnknownCalcToken ? 1 : 0);
    writer.WriteU8(state.hasUnknownDrawToken ? 1 : 0);
    for (i32 value : state.reimuABombProjectilesState)
    {
        writer.WriteI32(value);
    }
    for (f32 value : state.reimuABombProjectilesRelated)
    {
        writer.WriteF32(value);
    }
    for (const auto &value : state.bombRegionPositions)
    {
        writer.WriteVec3(value);
    }
    for (const auto &value : state.bombRegionVelocities)
    {
        writer.WriteVec3(value);
    }
    for (const auto &ring : state.sprites)
    {
        for (const auto &sprite : ring)
        {
            WritePortableAnmVmState(writer, sprite);
        }
    }
}

bool ReadPortablePlayerBomb(BufferReader &reader, PortablePlayerBombState &state)
{
    u16 calcToken = 0;
    u16 drawToken = 0;
    u8 hasUnknownCalc = 0;
    u8 hasUnknownDraw = 0;
    if (!reader.ReadU32(state.isInUse) || !reader.ReadI32(state.duration) || !reader.ReadTimer(state.timer) ||
        !reader.ReadU16(calcToken) || !reader.ReadU16(drawToken) || !reader.ReadU8(hasUnknownCalc) ||
        !reader.ReadU8(hasUnknownDraw))
    {
        return false;
    }
    state.calcToken = (PortablePlayerBombFuncToken)calcToken;
    state.drawToken = (PortablePlayerBombFuncToken)drawToken;
    state.hasUnknownCalcToken = hasUnknownCalc != 0;
    state.hasUnknownDrawToken = hasUnknownDraw != 0;
    for (i32 &value : state.reimuABombProjectilesState)
    {
        if (!reader.ReadI32(value))
            return false;
    }
    for (f32 &value : state.reimuABombProjectilesRelated)
    {
        if (!reader.ReadF32(value))
            return false;
    }
    for (auto &value : state.bombRegionPositions)
    {
        if (!reader.ReadVec3(value))
            return false;
    }
    for (auto &value : state.bombRegionVelocities)
    {
        if (!reader.ReadVec3(value))
            return false;
    }
    for (auto &ring : state.sprites)
    {
        for (auto &sprite : ring)
        {
            if (!ReadPortableAnmVmState(reader, sprite))
                return false;
        }
    }
    return true;
}

void WritePortableCharacterData(BufferWriter &writer, const PortableCharacterDataState &state)
{
    writer.WriteF32(state.orthogonalMovementSpeed);
    writer.WriteF32(state.orthogonalMovementSpeedFocus);
    writer.WriteF32(state.diagonalMovementSpeed);
    writer.WriteF32(state.diagonalMovementSpeedFocus);
    writer.WriteU16((u16)state.fireBulletToken);
    writer.WriteU16((u16)state.fireBulletFocusToken);
    writer.WriteU8(state.hasUnknownFireToken ? 1 : 0);
    writer.WriteU8(state.hasUnknownFocusFireToken ? 1 : 0);
}

bool ReadPortableCharacterData(BufferReader &reader, PortableCharacterDataState &state)
{
    u16 fireToken = 0;
    u16 focusToken = 0;
    u8 hasUnknownFire = 0;
    u8 hasUnknownFocus = 0;
    if (!reader.ReadF32(state.orthogonalMovementSpeed) || !reader.ReadF32(state.orthogonalMovementSpeedFocus) ||
        !reader.ReadF32(state.diagonalMovementSpeed) || !reader.ReadF32(state.diagonalMovementSpeedFocus) ||
        !reader.ReadU16(fireToken) || !reader.ReadU16(focusToken) || !reader.ReadU8(hasUnknownFire) ||
        !reader.ReadU8(hasUnknownFocus))
    {
        return false;
    }
    state.fireBulletToken = (PortablePlayerFireFuncToken)fireToken;
    state.fireBulletFocusToken = (PortablePlayerFireFuncToken)focusToken;
    state.hasUnknownFireToken = hasUnknownFire != 0;
    state.hasUnknownFocusFireToken = hasUnknownFocus != 0;
    return true;
}

void WritePlayerRect(BufferWriter &writer, const PlayerRect &rect)
{
    writer.WriteF32(rect.posX);
    writer.WriteF32(rect.posY);
    writer.WriteF32(rect.sizeX);
    writer.WriteF32(rect.sizeY);
}

bool ReadPlayerRect(BufferReader &reader, PlayerRect &rect)
{
    return reader.ReadF32(rect.posX) && reader.ReadF32(rect.posY) && reader.ReadF32(rect.sizeX) &&
           reader.ReadF32(rect.sizeY);
}

void WritePortablePlayerState(BufferWriter &writer, const PortablePlayerState &state)
{
    writer.WriteU8(state.isPresent ? 1 : 0);
    WritePortableAnmVmState(writer, state.playerSprite);
    for (const auto &sprite : state.orbsSprite)
    {
        WritePortableAnmVmState(writer, sprite);
    }
    writer.WriteVec3(state.positionCenter);
    writer.WriteVec3(state.unk_44c);
    writer.WriteVec3(state.hitboxTopLeft);
    writer.WriteVec3(state.hitboxBottomRight);
    writer.WriteVec3(state.grabItemTopLeft);
    writer.WriteVec3(state.grabItemBottomRight);
    writer.WriteVec3(state.hitboxSize);
    writer.WriteVec3(state.grabItemSize);
    for (const auto &value : state.orbsPosition)
    {
        writer.WriteVec3(value);
    }
    for (const auto &value : state.bombRegionPositions)
    {
        writer.WriteVec3(value);
    }
    for (const auto &value : state.bombRegionSizes)
    {
        writer.WriteVec3(value);
    }
    for (i32 value : state.bombRegionDamages)
    {
        writer.WriteI32(value);
    }
    for (i32 value : state.unk_838)
    {
        writer.WriteI32(value);
    }
    for (const auto &rect : state.bombProjectiles)
    {
        WritePlayerRect(writer, rect);
    }
    for (const auto &timer : state.laserTimer)
    {
        writer.WriteTimer(timer);
    }
    writer.WriteF32(state.horizontalMovementSpeedMultiplierDuringBomb);
    writer.WriteF32(state.verticalMovementSpeedMultiplierDuringBomb);
    writer.WriteI32(state.respawnTimer);
    writer.WriteI32(state.bulletGracePeriod);
    writer.WriteU8((u8)state.playerState);
    writer.WriteU8((u8)state.playerType);
    writer.WriteU8(state.unk_9e1);
    writer.WriteU8((u8)state.orbState);
    writer.WriteU8((u8)state.isFocus);
    writer.WriteU8(state.unk_9e4);
    writer.WriteTimer(state.focusMovementTimer);
    WritePortableCharacterData(writer, state.characterData);
    writer.WriteI32(state.playerDirection);
    writer.WriteF32(state.previousHorizontalSpeed);
    writer.WriteF32(state.previousVerticalSpeed);
    writer.WriteI16(state.previousFrameInput);
    writer.WriteVec3(state.positionOfLastEnemyHit);
    for (const auto &bullet : state.bullets)
    {
        WritePortablePlayerBullet(writer, bullet);
    }
    writer.WriteTimer(state.fireBulletTimer);
    writer.WriteTimer(state.invulnerabilityTimer);
    writer.WriteU16((u16)state.fireBulletToken);
    writer.WriteU16((u16)state.fireBulletFocusToken);
    writer.WriteU8(state.hasUnknownFireToken ? 1 : 0);
    writer.WriteU8(state.hasUnknownFocusFireToken ? 1 : 0);
    WritePortablePlayerBomb(writer, state.bombInfo);
    WritePortableAnmVmState(writer, state.hitboxSprite);
    writer.WriteI32(state.hitboxTime);
    writer.WriteI32(state.lifegiveTime);
}

bool ReadPortablePlayerState(BufferReader &reader, PortablePlayerState &state)
{
    u8 isPresent = 0;
    u16 fireToken = 0;
    u16 focusToken = 0;
    u8 hasUnknownFire = 0;
    u8 hasUnknownFocus = 0;
    if (!reader.ReadU8(isPresent) || !ReadPortableAnmVmState(reader, state.playerSprite))
    {
        return false;
    }
    state.isPresent = isPresent != 0;
    for (auto &sprite : state.orbsSprite)
    {
        if (!ReadPortableAnmVmState(reader, sprite))
            return false;
    }
    if (!reader.ReadVec3(state.positionCenter) || !reader.ReadVec3(state.unk_44c) ||
        !reader.ReadVec3(state.hitboxTopLeft) || !reader.ReadVec3(state.hitboxBottomRight) ||
        !reader.ReadVec3(state.grabItemTopLeft) || !reader.ReadVec3(state.grabItemBottomRight) ||
        !reader.ReadVec3(state.hitboxSize) || !reader.ReadVec3(state.grabItemSize))
    {
        return false;
    }
    for (auto &value : state.orbsPosition)
    {
        if (!reader.ReadVec3(value))
            return false;
    }
    for (auto &value : state.bombRegionPositions)
    {
        if (!reader.ReadVec3(value))
            return false;
    }
    for (auto &value : state.bombRegionSizes)
    {
        if (!reader.ReadVec3(value))
            return false;
    }
    for (i32 &value : state.bombRegionDamages)
    {
        if (!reader.ReadI32(value))
            return false;
    }
    for (i32 &value : state.unk_838)
    {
        if (!reader.ReadI32(value))
            return false;
    }
    for (auto &rect : state.bombProjectiles)
    {
        if (!ReadPlayerRect(reader, rect))
            return false;
    }
    for (auto &timer : state.laserTimer)
    {
        if (!reader.ReadTimer(timer))
            return false;
    }
    if (!reader.ReadF32(state.horizontalMovementSpeedMultiplierDuringBomb) ||
        !reader.ReadF32(state.verticalMovementSpeedMultiplierDuringBomb) || !reader.ReadI32(state.respawnTimer) ||
        !reader.ReadI32(state.bulletGracePeriod))
    {
        return false;
    }
    if (!reader.ReadU8((u8 &)state.playerState) || !reader.ReadU8((u8 &)state.playerType) ||
        !reader.ReadU8(state.unk_9e1) || !reader.ReadU8((u8 &)state.orbState) || !reader.ReadU8((u8 &)state.isFocus) ||
        !reader.ReadU8(state.unk_9e4) || !reader.ReadTimer(state.focusMovementTimer) ||
        !ReadPortableCharacterData(reader, state.characterData) || !reader.ReadI32(state.playerDirection) ||
        !reader.ReadF32(state.previousHorizontalSpeed) || !reader.ReadF32(state.previousVerticalSpeed) ||
        !reader.ReadI16(state.previousFrameInput) || !reader.ReadVec3(state.positionOfLastEnemyHit))
    {
        return false;
    }
    for (auto &bullet : state.bullets)
    {
        if (!ReadPortablePlayerBullet(reader, bullet))
            return false;
    }
    if (!reader.ReadTimer(state.fireBulletTimer) || !reader.ReadTimer(state.invulnerabilityTimer) ||
        !reader.ReadU16(fireToken) || !reader.ReadU16(focusToken) || !reader.ReadU8(hasUnknownFire) ||
        !reader.ReadU8(hasUnknownFocus))
    {
        return false;
    }
    state.fireBulletToken = (PortablePlayerFireFuncToken)fireToken;
    state.fireBulletFocusToken = (PortablePlayerFireFuncToken)focusToken;
    state.hasUnknownFireToken = hasUnknownFire != 0;
    state.hasUnknownFocusFireToken = hasUnknownFocus != 0;
    return ReadPortablePlayerBomb(reader, state.bombInfo) && ReadPortableAnmVmState(reader, state.hitboxSprite) &&
           reader.ReadI32(state.hitboxTime) && reader.ReadI32(state.lifegiveTime);
}

void WritePortableBulletTypeSprites(BufferWriter &writer, const PortableBulletTypeSpritesState &state)
{
    WritePortableAnmVmState(writer, state.spriteBullet);
    WritePortableAnmVmState(writer, state.spriteSpawnEffectFast);
    WritePortableAnmVmState(writer, state.spriteSpawnEffectNormal);
    WritePortableAnmVmState(writer, state.spriteSpawnEffectSlow);
    WritePortableAnmVmState(writer, state.spriteSpawnEffectDonut);
    writer.WriteVec3(state.grazeSize);
    writer.WriteU8(state.unk_55c);
    writer.WriteU8(state.bulletHeight);
}

bool ReadPortableBulletTypeSprites(BufferReader &reader, PortableBulletTypeSpritesState &state)
{
    return ReadPortableAnmVmState(reader, state.spriteBullet) &&
           ReadPortableAnmVmState(reader, state.spriteSpawnEffectFast) &&
           ReadPortableAnmVmState(reader, state.spriteSpawnEffectNormal) &&
           ReadPortableAnmVmState(reader, state.spriteSpawnEffectSlow) &&
           ReadPortableAnmVmState(reader, state.spriteSpawnEffectDonut) && reader.ReadVec3(state.grazeSize) &&
           reader.ReadU8(state.unk_55c) && reader.ReadU8(state.bulletHeight);
}

void WritePortableBulletState(BufferWriter &writer, const PortableBulletState &state)
{
    WritePortableBulletTypeSprites(writer, state.sprites);
    writer.WriteVec3(state.pos);
    writer.WriteVec3(state.velocity);
    writer.WriteVec3(state.ex4Acceleration);
    writer.WriteF32(state.speed);
    writer.WriteF32(state.ex5Float0);
    writer.WriteF32(state.dirChangeSpeed);
    writer.WriteF32(state.angle);
    writer.WriteF32(state.ex5Float1);
    writer.WriteF32(state.dirChangeRotation);
    writer.WriteTimer(state.timer);
    writer.WriteI32(state.ex5Int0);
    writer.WriteI32(state.dirChangeInterval);
    writer.WriteI32(state.dirChangeNumTimes);
    writer.WriteI32(state.dirChangeMaxTimes);
    writer.WriteU16(state.exFlags);
    writer.WriteI16(state.spriteOffset);
    writer.WriteU16(state.unk_5bc);
    writer.WriteU16(state.state);
    writer.WriteU16(state.unk_5c0);
    writer.WriteU8(state.unk_5c2);
    writer.WriteU8(state.isGrazed);
    writer.WriteU8(state.provokedPlayer);
}

bool ReadPortableBulletState(BufferReader &reader, PortableBulletState &state)
{
    return ReadPortableBulletTypeSprites(reader, state.sprites) && reader.ReadVec3(state.pos) &&
           reader.ReadVec3(state.velocity) && reader.ReadVec3(state.ex4Acceleration) && reader.ReadF32(state.speed) &&
           reader.ReadF32(state.ex5Float0) && reader.ReadF32(state.dirChangeSpeed) && reader.ReadF32(state.angle) &&
           reader.ReadF32(state.ex5Float1) && reader.ReadF32(state.dirChangeRotation) &&
           reader.ReadTimer(state.timer) && reader.ReadI32(state.ex5Int0) && reader.ReadI32(state.dirChangeInterval) &&
           reader.ReadI32(state.dirChangeNumTimes) && reader.ReadI32(state.dirChangeMaxTimes) &&
           reader.ReadU16(state.exFlags) && reader.ReadI16(state.spriteOffset) && reader.ReadU16(state.unk_5bc) &&
           reader.ReadU16(state.state) && reader.ReadU16(state.unk_5c0) && reader.ReadU8(state.unk_5c2) &&
           reader.ReadU8(state.isGrazed) && reader.ReadU8(state.provokedPlayer);
}

void WritePortableLaserState(BufferWriter &writer, const PortableLaserState &state)
{
    WritePortableAnmVmState(writer, state.vm0);
    WritePortableAnmVmState(writer, state.vm1);
    writer.WriteVec3(state.pos);
    writer.WriteF32(state.angle);
    writer.WriteF32(state.startOffset);
    writer.WriteF32(state.endOffset);
    writer.WriteF32(state.startLength);
    writer.WriteF32(state.width);
    writer.WriteF32(state.speed);
    writer.WriteI32(state.startTime);
    writer.WriteI32(state.hitboxStartTime);
    writer.WriteI32(state.duration);
    writer.WriteI32(state.despawnDuration);
    writer.WriteI32(state.hitboxEndDelay);
    writer.WriteI32(state.inUse);
    writer.WriteTimer(state.timer);
    writer.WriteU16(state.flags);
    writer.WriteI16(state.color);
    writer.WriteU8(state.state);
    writer.WriteU8(state.provokedPlayer);
}

bool ReadPortableLaserState(BufferReader &reader, PortableLaserState &state)
{
    return ReadPortableAnmVmState(reader, state.vm0) && ReadPortableAnmVmState(reader, state.vm1) &&
           reader.ReadVec3(state.pos) && reader.ReadF32(state.angle) && reader.ReadF32(state.startOffset) &&
           reader.ReadF32(state.endOffset) && reader.ReadF32(state.startLength) && reader.ReadF32(state.width) &&
           reader.ReadF32(state.speed) && reader.ReadI32(state.startTime) && reader.ReadI32(state.hitboxStartTime) &&
           reader.ReadI32(state.duration) && reader.ReadI32(state.despawnDuration) &&
           reader.ReadI32(state.hitboxEndDelay) && reader.ReadI32(state.inUse) && reader.ReadTimer(state.timer) &&
           reader.ReadU16(state.flags) && reader.ReadI16(state.color) && reader.ReadU8(state.state) &&
           reader.ReadU8(state.provokedPlayer);
}

void WritePortableBulletManagerState(BufferWriter &writer, const PortableBulletManagerState &state)
{
    writer.WriteString(state.bulletAnmPath);
    for (const auto &templateState : state.bulletTypeTemplates)
    {
        WritePortableBulletTypeSprites(writer, templateState);
    }
    for (const auto &bullet : state.bullets)
    {
        WritePortableBulletState(writer, bullet);
    }
    for (const auto &laser : state.lasers)
    {
        WritePortableLaserState(writer, laser);
    }
    writer.WriteI32(state.nextBulletIndex);
    writer.WriteI32(state.bulletCount);
    writer.WriteTimer(state.time);
}

bool ReadPortableBulletManagerState(BufferReader &reader, PortableBulletManagerState &state)
{
    if (!reader.ReadString(state.bulletAnmPath))
    {
        return false;
    }
    for (auto &templateState : state.bulletTypeTemplates)
    {
        if (!ReadPortableBulletTypeSprites(reader, templateState))
            return false;
    }
    for (auto &bullet : state.bullets)
    {
        if (!ReadPortableBulletState(reader, bullet))
            return false;
    }
    for (auto &laser : state.lasers)
    {
        if (!ReadPortableLaserState(reader, laser))
            return false;
    }
    return reader.ReadI32(state.nextBulletIndex) && reader.ReadI32(state.bulletCount) && reader.ReadTimer(state.time);
}

void WriteQuaternion(BufferWriter &writer, const D3DXQUATERNION &quat)
{
    writer.WriteF32(quat.x);
    writer.WriteF32(quat.y);
    writer.WriteF32(quat.z);
    writer.WriteF32(quat.w);
}

bool ReadQuaternion(BufferReader &reader, D3DXQUATERNION &quat)
{
    return reader.ReadF32(quat.x) && reader.ReadF32(quat.y) && reader.ReadF32(quat.z) && reader.ReadF32(quat.w);
}

void WritePortableItemState(BufferWriter &writer, const PortableItemState &state)
{
    WritePortableAnmVmState(writer, state.sprite);
    writer.WriteVec3(state.currentPosition);
    writer.WriteVec3(state.startPosition);
    writer.WriteVec3(state.targetPosition);
    writer.WriteTimer(state.timer);
    writer.WriteU8((u8)state.itemType);
    writer.WriteU8((u8)state.isInUse);
    writer.WriteU8((u8)state.unk_142);
    writer.WriteU8((u8)state.state);
}

bool ReadPortableItemState(BufferReader &reader, PortableItemState &state)
{
    return ReadPortableAnmVmState(reader, state.sprite) && reader.ReadVec3(state.currentPosition) &&
           reader.ReadVec3(state.startPosition) && reader.ReadVec3(state.targetPosition) &&
           reader.ReadTimer(state.timer) && reader.ReadU8((u8 &)state.itemType) && reader.ReadU8((u8 &)state.isInUse) &&
           reader.ReadU8((u8 &)state.unk_142) && reader.ReadU8((u8 &)state.state);
}

void WritePortableItemManagerState(BufferWriter &writer, const PortableItemManagerState &state)
{
    for (const auto &item : state.items)
    {
        WritePortableItemState(writer, item);
    }
    writer.WriteI32(state.nextIndex);
    writer.WriteU32(state.itemCount);
}

bool ReadPortableItemManagerState(BufferReader &reader, PortableItemManagerState &state)
{
    for (auto &item : state.items)
    {
        if (!ReadPortableItemState(reader, item))
        {
            return false;
        }
    }
    return reader.ReadI32(state.nextIndex) && reader.ReadU32(state.itemCount);
}

void WritePortableEffectState(BufferWriter &writer, const PortableEffectState &state)
{
    WritePortableAnmVmState(writer, state.vm);
    writer.WriteVec3(state.pos1);
    writer.WriteVec3(state.unk_11c);
    writer.WriteVec3(state.unk_128);
    writer.WriteVec3(state.position);
    writer.WriteVec3(state.pos2);
    WriteQuaternion(writer, state.quaternion);
    writer.WriteF32(state.unk_15c);
    writer.WriteF32(state.angleRelated);
    writer.WriteTimer(state.timer);
    writer.WriteI32(state.unk_170);
    writer.WriteU16((u16)state.updateCallbackToken);
    writer.WriteU8(state.hasUnknownUpdateToken ? 1 : 0);
    writer.WriteU8((u8)state.inUseFlag);
    writer.WriteU8((u8)state.effectId);
    writer.WriteU8((u8)state.unk_17a);
    writer.WriteU8((u8)state.unk_17b);
}

bool ReadPortableEffectState(BufferReader &reader, PortableEffectState &state)
{
    u16 token = 0;
    u8 hasUnknown = 0;
    if (!ReadPortableAnmVmState(reader, state.vm) || !reader.ReadVec3(state.pos1) || !reader.ReadVec3(state.unk_11c) ||
        !reader.ReadVec3(state.unk_128) || !reader.ReadVec3(state.position) || !reader.ReadVec3(state.pos2) ||
        !ReadQuaternion(reader, state.quaternion) || !reader.ReadF32(state.unk_15c) ||
        !reader.ReadF32(state.angleRelated) || !reader.ReadTimer(state.timer) || !reader.ReadI32(state.unk_170) ||
        !reader.ReadU16(token) || !reader.ReadU8(hasUnknown) || !reader.ReadU8((u8 &)state.inUseFlag) ||
        !reader.ReadU8((u8 &)state.effectId) || !reader.ReadU8((u8 &)state.unk_17a) ||
        !reader.ReadU8((u8 &)state.unk_17b))
    {
        return false;
    }
    state.updateCallbackToken = (PortableEffectUpdateToken)token;
    state.hasUnknownUpdateToken = hasUnknown != 0;
    return true;
}

void WritePortableEffectManagerState(BufferWriter &writer, const PortableEffectManagerState &state)
{
    writer.WriteI32(state.nextIndex);
    writer.WriteI32(state.activeEffects);
    for (const auto &effect : state.effects)
    {
        WritePortableEffectState(writer, effect);
    }
}

bool ReadPortableEffectManagerState(BufferReader &reader, PortableEffectManagerState &state)
{
    if (!reader.ReadI32(state.nextIndex) || !reader.ReadI32(state.activeEffects))
    {
        return false;
    }
    for (auto &effect : state.effects)
    {
        if (!ReadPortableEffectState(reader, effect))
        {
            return false;
        }
    }
    return true;
}

void WritePortableEnemyState(BufferWriter &writer, const PortableEnemyState &state)
{
    WritePortableAnmVmState(writer, state.primaryVm);
    for (const auto &vm : state.vms)
    {
        WritePortableAnmVmState(writer, vm);
    }
    WritePortableEnemyContext(writer, state.currentContext);
    for (const auto &context : state.savedContextStack)
    {
        WritePortableEnemyContext(writer, context);
    }
    writer.WriteI32(state.stackDepth);
    writer.WriteI32(state.unk_c40);
    writer.WriteI32(state.deathCallbackSub);
    for (i32 value : state.interrupts)
    {
        writer.WriteI32(value);
    }
    writer.WriteI32(state.runInterrupt);
    writer.WriteVec3(state.position);
    writer.WriteVec3(state.hitboxDimensions);
    writer.WriteVec3(state.axisSpeed);
    writer.WriteF32(state.angle);
    writer.WriteF32(state.angularVelocity);
    writer.WriteF32(state.speed);
    writer.WriteF32(state.acceleration);
    writer.WriteVec3(state.shootOffset);
    writer.WriteVec3(state.moveInterp);
    writer.WriteVec3(state.moveInterpStartPos);
    writer.WriteTimer(state.moveInterpTimer);
    writer.WriteI32(state.moveInterpStartTime);
    writer.WriteF32(state.bulletRankSpeedLow);
    writer.WriteF32(state.bulletRankSpeedHigh);
    writer.WriteI16(state.bulletRankAmount1Low);
    writer.WriteI16(state.bulletRankAmount1High);
    writer.WriteI16(state.bulletRankAmount2Low);
    writer.WriteI16(state.bulletRankAmount2High);
    writer.WriteI32(state.life);
    writer.WriteI32(state.maxLife);
    writer.WriteI32(state.score);
    writer.WriteTimer(state.bossTimer);
    writer.WriteU32(state.color);
    WritePortableEnemyBulletShooter(writer, state.bulletProps);
    writer.WriteI32(state.shootInterval);
    writer.WriteTimer(state.shootIntervalTimer);
    WritePortableEnemyLaserShooter(writer, state.laserProps);
    for (i32 value : state.laserIndices)
    {
        writer.WriteI32(value);
    }
    writer.WriteI32(state.laserStore);
    writer.WriteU8(state.deathAnm1);
    writer.WriteU8(state.deathAnm2);
    writer.WriteU8(state.deathAnm3);
    writer.WriteU8((u8)state.itemDrop);
    writer.WriteU8(state.bossId);
    writer.WriteU8(state.unk_e41);
    writer.WriteTimer(state.exInsFunc10Timer);
    WritePortableEnemyFlags(writer, state.flags);
    writer.WriteU8(state.anmExFlags);
    writer.WriteI16(state.anmExDefaults);
    writer.WriteI16(state.anmExFarLeft);
    writer.WriteI16(state.anmExFarRight);
    writer.WriteI16(state.anmExLeft);
    writer.WriteI16(state.anmExRight);
    writer.WriteVec2(state.lowerMoveLimit);
    writer.WriteVec2(state.upperMoveLimit);
    for (i32 value : state.effectIndices)
    {
        writer.WriteI32(value);
    }
    writer.WriteI32(state.effectIdx);
    writer.WriteF32(state.effectDistance);
    writer.WriteI32(state.lifeCallbackThreshold);
    writer.WriteI32(state.lifeCallbackSub);
    writer.WriteI32(state.timerCallbackThreshold);
    writer.WriteI32(state.timerCallbackSub);
    writer.WriteF32(state.exInsFunc6Angle);
    writer.WriteTimer(state.exInsFunc6Timer);
    writer.WriteU8(state.provokedPlayer);
}

bool ReadPortableEnemyState(BufferReader &reader, PortableEnemyState &state)
{
    if (!ReadPortableAnmVmState(reader, state.primaryVm))
    {
        return false;
    }
    for (auto &vm : state.vms)
    {
        if (!ReadPortableAnmVmState(reader, vm))
        {
            return false;
        }
    }
    if (!ReadPortableEnemyContext(reader, state.currentContext))
    {
        return false;
    }
    for (auto &context : state.savedContextStack)
    {
        if (!ReadPortableEnemyContext(reader, context))
        {
            return false;
        }
    }
    if (!reader.ReadI32(state.stackDepth) || !reader.ReadI32(state.unk_c40) ||
        !reader.ReadI32(state.deathCallbackSub))
    {
        return false;
    }
    for (i32 &value : state.interrupts)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    if (!reader.ReadI32(state.runInterrupt) || !reader.ReadVec3(state.position) ||
        !reader.ReadVec3(state.hitboxDimensions) || !reader.ReadVec3(state.axisSpeed) ||
        !reader.ReadF32(state.angle) || !reader.ReadF32(state.angularVelocity) ||
        !reader.ReadF32(state.speed) || !reader.ReadF32(state.acceleration) ||
        !reader.ReadVec3(state.shootOffset) || !reader.ReadVec3(state.moveInterp) ||
        !reader.ReadVec3(state.moveInterpStartPos) || !reader.ReadTimer(state.moveInterpTimer) ||
        !reader.ReadI32(state.moveInterpStartTime) || !reader.ReadF32(state.bulletRankSpeedLow) ||
        !reader.ReadF32(state.bulletRankSpeedHigh) || !reader.ReadI16(state.bulletRankAmount1Low) ||
        !reader.ReadI16(state.bulletRankAmount1High) || !reader.ReadI16(state.bulletRankAmount2Low) ||
        !reader.ReadI16(state.bulletRankAmount2High) || !reader.ReadI32(state.life) ||
        !reader.ReadI32(state.maxLife) || !reader.ReadI32(state.score) || !reader.ReadTimer(state.bossTimer) ||
        !reader.ReadU32(state.color) || !ReadPortableEnemyBulletShooter(reader, state.bulletProps) ||
        !reader.ReadI32(state.shootInterval) || !reader.ReadTimer(state.shootIntervalTimer) ||
        !ReadPortableEnemyLaserShooter(reader, state.laserProps))
    {
        return false;
    }
    for (i32 &value : state.laserIndices)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    u8 itemDrop = 0;
    if (!reader.ReadI32(state.laserStore) || !reader.ReadU8(state.deathAnm1) || !reader.ReadU8(state.deathAnm2) ||
        !reader.ReadU8(state.deathAnm3) || !reader.ReadU8(itemDrop) || !reader.ReadU8(state.bossId) ||
        !reader.ReadU8(state.unk_e41) || !reader.ReadTimer(state.exInsFunc10Timer) ||
        !ReadPortableEnemyFlags(reader, state.flags) || !reader.ReadU8(state.anmExFlags) ||
        !reader.ReadI16(state.anmExDefaults) || !reader.ReadI16(state.anmExFarLeft) ||
        !reader.ReadI16(state.anmExFarRight) || !reader.ReadI16(state.anmExLeft) ||
        !reader.ReadI16(state.anmExRight) || !reader.ReadVec2(state.lowerMoveLimit) ||
        !reader.ReadVec2(state.upperMoveLimit))
    {
        return false;
    }
    state.itemDrop = (i8)itemDrop;
    for (i32 &value : state.effectIndices)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    return reader.ReadI32(state.effectIdx) && reader.ReadF32(state.effectDistance) &&
           reader.ReadI32(state.lifeCallbackThreshold) && reader.ReadI32(state.lifeCallbackSub) &&
           reader.ReadI32(state.timerCallbackThreshold) && reader.ReadI32(state.timerCallbackSub) &&
           reader.ReadF32(state.exInsFunc6Angle) && reader.ReadTimer(state.exInsFunc6Timer) &&
           reader.ReadU8(state.provokedPlayer);
}

void EncodeCatalogSection(BufferWriter &writer, const PortableGameplayResourceCatalog &catalog)
{
    writer.WriteU64(catalog.catalogHash);
    writer.WriteU32(catalog.currentStage);
    writer.WriteU32(catalog.difficulty);
    writer.WriteU32(catalog.character1);
    writer.WriteU32(catalog.shotType1);
    writer.WriteU32(catalog.character2);
    writer.WriteU32(catalog.shotType2);
    writer.WriteU32(catalog.hasSecondPlayer);
    writer.WriteU32(catalog.isPracticeMode);
    writer.WriteU32(catalog.isReplay);
}

bool DecodeCatalogSection(BufferReader &reader, PortableGameplayResourceCatalog &catalog)
{
    return reader.ReadU64(catalog.catalogHash) && reader.ReadU32(catalog.currentStage) &&
           reader.ReadU32(catalog.difficulty) && reader.ReadU32(catalog.character1) &&
           reader.ReadU32(catalog.shotType1) && reader.ReadU32(catalog.character2) &&
           reader.ReadU32(catalog.shotType2) && reader.ReadU32(catalog.hasSecondPlayer) &&
           reader.ReadU32(catalog.isPracticeMode) && reader.ReadU32(catalog.isReplay);
}

void EncodeCoreSection(BufferWriter &writer, const PortableGameplayCoreState &core)
{
    writer.WriteU32(core.guiScore);
    writer.WriteU32(core.score);
    writer.WriteU32(core.nextScoreIncrement);
    writer.WriteU32(core.highScore);
    writer.WriteU32(core.difficulty);
    writer.WriteI32(core.grazeInStage);
    writer.WriteI32(core.grazeInTotal);
    writer.WriteI32(core.deaths);
    writer.WriteI32(core.bombsUsed);
    writer.WriteI32(core.spellcardsCaptured);
    writer.WriteI32(core.pointItemsCollectedInStage);
    writer.WriteI32(core.pointItemsCollected);
    writer.WriteI32(core.powerItemCountForScore);
    writer.WriteI32(core.extraLives);
    writer.WriteI32(core.currentStage);
    writer.WriteI32(core.rank);
    writer.WriteI32(core.maxRank);
    writer.WriteI32(core.minRank);
    writer.WriteI32(core.subRank);
    writer.WriteU32(core.randomSeed);
    writer.WriteU32(core.gameFrames);
    writer.WriteU16(core.currentPower1);
    writer.WriteU16(core.currentPower2);
    writer.WriteI16(core.livesRemaining1);
    writer.WriteI16(core.bombsRemaining1);
    writer.WriteI16(core.livesRemaining2);
    writer.WriteI16(core.bombsRemaining2);
    writer.WriteU16(core.numRetries);
    writer.WriteU16(core.isTimeStopped);
    writer.WriteU16(core.isGameCompleted);
    writer.WriteU16(core.isPracticeMode);
    writer.WriteU16(core.demoMode);
    writer.WriteU8(core.character1);
    writer.WriteU8(core.shotType1);
    writer.WriteU8(core.character2);
    writer.WriteU8(core.shotType2);
}

bool DecodeCoreSection(BufferReader &reader, PortableGameplayCoreState &core)
{
    return reader.ReadU32(core.guiScore) && reader.ReadU32(core.score) && reader.ReadU32(core.nextScoreIncrement) &&
           reader.ReadU32(core.highScore) && reader.ReadU32(core.difficulty) && reader.ReadI32(core.grazeInStage) &&
           reader.ReadI32(core.grazeInTotal) && reader.ReadI32(core.deaths) && reader.ReadI32(core.bombsUsed) &&
           reader.ReadI32(core.spellcardsCaptured) && reader.ReadI32(core.pointItemsCollectedInStage) &&
           reader.ReadI32(core.pointItemsCollected) && reader.ReadI32(core.powerItemCountForScore) &&
           reader.ReadI32(core.extraLives) && reader.ReadI32(core.currentStage) && reader.ReadI32(core.rank) &&
           reader.ReadI32(core.maxRank) && reader.ReadI32(core.minRank) && reader.ReadI32(core.subRank) &&
           reader.ReadU32(core.randomSeed) && reader.ReadU32(core.gameFrames) &&
           reader.ReadU16(core.currentPower1) && reader.ReadU16(core.currentPower2) &&
           reader.ReadI16(core.livesRemaining1) && reader.ReadI16(core.bombsRemaining1) &&
           reader.ReadI16(core.livesRemaining2) && reader.ReadI16(core.bombsRemaining2) &&
           reader.ReadU16(core.numRetries) && reader.ReadU16(core.isTimeStopped) &&
           reader.ReadU16(core.isGameCompleted) && reader.ReadU16(core.isPracticeMode) &&
           reader.ReadU16(core.demoMode) && reader.ReadU8(core.character1) && reader.ReadU8(core.shotType1) &&
           reader.ReadU8(core.character2) && reader.ReadU8(core.shotType2);
}

void EncodeRuntimeSection(BufferWriter &writer, const PortableGameplayRuntimeState &runtime)
{
    writer.WriteI32(runtime.frame);
    writer.WriteI32(runtime.stage);
    writer.WriteI32(runtime.delay);
    writer.WriteI32(runtime.currentDelayCooldown);

    writer.WriteI32(runtime.enemyEclRuntimeState.playerShot);
    writer.WriteF32(runtime.enemyEclRuntimeState.playerDistance);
    writer.WriteF32(runtime.enemyEclRuntimeState.playerAngle);
    for (f32 angle : runtime.enemyEclRuntimeState.starAngleTable)
    {
        writer.WriteF32(angle);
    }
    writer.WriteVec3(runtime.enemyEclRuntimeState.enemyPosVector);
    writer.WriteVec3(runtime.enemyEclRuntimeState.playerPosVector);
    for (i32 value : runtime.enemyEclRuntimeState.eclLiteralInts)
    {
        writer.WriteI32(value);
    }
    for (f32 value : runtime.enemyEclRuntimeState.eclLiteralFloats)
    {
        writer.WriteF32(value);
    }
    writer.WriteI32(runtime.enemyEclRuntimeState.eclLiteralIntCursor);
    writer.WriteI32(runtime.enemyEclRuntimeState.eclLiteralFloatCursor);

    writer.WriteU32((u32)runtime.screenEffectRuntimeState.activeEffects.size());
    for (const auto &effect : runtime.screenEffectRuntimeState.activeEffects)
    {
        WriteRuntimeEffectState(writer, effect);
    }

    writer.WriteU16(runtime.controllerRuntimeState.focusButtonConflictState);

    writer.WriteI32(runtime.supervisorRuntimeState.calcCount);
    writer.WriteI32(runtime.supervisorRuntimeState.wantedState);
    writer.WriteI32(runtime.supervisorRuntimeState.curState);
    writer.WriteI32(runtime.supervisorRuntimeState.wantedState2);
    writer.WriteI32(runtime.supervisorRuntimeState.unk194);
    writer.WriteI32(runtime.supervisorRuntimeState.unk198);
    writer.WriteU8(runtime.supervisorRuntimeState.isInEnding ? 1 : 0);
    writer.WriteI32(runtime.supervisorRuntimeState.vsyncEnabled);
    writer.WriteI32(runtime.supervisorRuntimeState.lastFrameTime);
    writer.WriteF32(runtime.supervisorRuntimeState.effectiveFramerateMultiplier);
    writer.WriteF32(runtime.supervisorRuntimeState.framerateMultiplier);
    writer.WriteF32(runtime.supervisorRuntimeState.unk1b4);
    writer.WriteF32(runtime.supervisorRuntimeState.unk1b8);
    writer.WriteU32(runtime.supervisorRuntimeState.startupTimeBeforeMenuMusic);

    writer.WriteI32(runtime.gameWindowRuntimeState.tickCountToEffectiveFramerate);
    writer.WriteF64(runtime.gameWindowRuntimeState.lastFrameTime);
    writer.WriteU8(runtime.gameWindowRuntimeState.curFrame);

    for (i32 value : runtime.soundRuntimeState.soundBuffersToPlay)
    {
        writer.WriteI32(value);
    }
    for (i32 value : runtime.soundRuntimeState.queuedSfxState)
    {
        writer.WriteI32(value);
    }
    writer.WriteI32(runtime.soundRuntimeState.isLooping);

    writer.WriteU16(runtime.inputRuntimeState.lastFrameInput);
    writer.WriteU16(runtime.inputRuntimeState.curFrameInput);
    writer.WriteU16(runtime.inputRuntimeState.eighthFrameHeldInput);
    writer.WriteU16(runtime.inputRuntimeState.heldInputFrames);

    writer.WriteU32((u32)runtime.stageObjectInstances.size());
    for (const auto &instance : runtime.stageObjectInstances)
    {
        writer.WriteI16(instance.id);
        writer.WriteI16(instance.unk2);
        writer.WriteVec3(instance.position);
    }
}

bool DecodeRuntimeSection(BufferReader &reader, PortableGameplayRuntimeState &runtime)
{
    if (!reader.ReadI32(runtime.frame) || !reader.ReadI32(runtime.stage) || !reader.ReadI32(runtime.delay) ||
        !reader.ReadI32(runtime.currentDelayCooldown) ||
        !reader.ReadI32(runtime.enemyEclRuntimeState.playerShot) ||
        !reader.ReadF32(runtime.enemyEclRuntimeState.playerDistance) ||
        !reader.ReadF32(runtime.enemyEclRuntimeState.playerAngle))
    {
        return false;
    }

    for (f32 &angle : runtime.enemyEclRuntimeState.starAngleTable)
    {
        if (!reader.ReadF32(angle))
        {
            return false;
        }
    }
    if (!reader.ReadVec3(runtime.enemyEclRuntimeState.enemyPosVector) ||
        !reader.ReadVec3(runtime.enemyEclRuntimeState.playerPosVector))
    {
        return false;
    }
    for (i32 &value : runtime.enemyEclRuntimeState.eclLiteralInts)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    for (f32 &value : runtime.enemyEclRuntimeState.eclLiteralFloats)
    {
        if (!reader.ReadF32(value))
        {
            return false;
        }
    }
    if (!reader.ReadI32(runtime.enemyEclRuntimeState.eclLiteralIntCursor) ||
        !reader.ReadI32(runtime.enemyEclRuntimeState.eclLiteralFloatCursor))
    {
        return false;
    }

    u32 effectCount = 0;
    if (!reader.ReadU32(effectCount))
    {
        return false;
    }
    runtime.screenEffectRuntimeState.activeEffects.resize(effectCount);
    for (u32 i = 0; i < effectCount; ++i)
    {
        if (!ReadRuntimeEffectState(reader, runtime.screenEffectRuntimeState.activeEffects[i]))
        {
            return false;
        }
    }

    if (!reader.ReadU16(runtime.controllerRuntimeState.focusButtonConflictState) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.calcCount) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.wantedState) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.curState) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.wantedState2) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.unk194) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.unk198))
    {
        return false;
    }

    u8 isInEnding = 0;
    if (!reader.ReadU8(isInEnding) || !reader.ReadI32(runtime.supervisorRuntimeState.vsyncEnabled) ||
        !reader.ReadI32(runtime.supervisorRuntimeState.lastFrameTime) ||
        !reader.ReadF32(runtime.supervisorRuntimeState.effectiveFramerateMultiplier) ||
        !reader.ReadF32(runtime.supervisorRuntimeState.framerateMultiplier) ||
        !reader.ReadF32(runtime.supervisorRuntimeState.unk1b4) ||
        !reader.ReadF32(runtime.supervisorRuntimeState.unk1b8) ||
        !reader.ReadU32(runtime.supervisorRuntimeState.startupTimeBeforeMenuMusic))
    {
        return false;
    }
    runtime.supervisorRuntimeState.isInEnding = isInEnding != 0;

    if (!reader.ReadI32(runtime.gameWindowRuntimeState.tickCountToEffectiveFramerate) ||
        !reader.ReadF64(runtime.gameWindowRuntimeState.lastFrameTime) ||
        !reader.ReadU8(runtime.gameWindowRuntimeState.curFrame))
    {
        return false;
    }

    for (i32 &value : runtime.soundRuntimeState.soundBuffersToPlay)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    for (i32 &value : runtime.soundRuntimeState.queuedSfxState)
    {
        if (!reader.ReadI32(value))
        {
            return false;
        }
    }
    if (!reader.ReadI32(runtime.soundRuntimeState.isLooping) ||
        !reader.ReadU16(runtime.inputRuntimeState.lastFrameInput) ||
        !reader.ReadU16(runtime.inputRuntimeState.curFrameInput) ||
        !reader.ReadU16(runtime.inputRuntimeState.eighthFrameHeldInput) ||
        !reader.ReadU16(runtime.inputRuntimeState.heldInputFrames))
    {
        return false;
    }

    u32 objectCount = 0;
    if (!reader.ReadU32(objectCount))
    {
        return false;
    }
    runtime.stageObjectInstances.resize(objectCount);
    for (u32 i = 0; i < objectCount; ++i)
    {
        if (!reader.ReadI16(runtime.stageObjectInstances[i].id) || !reader.ReadI16(runtime.stageObjectInstances[i].unk2) ||
            !reader.ReadVec3(runtime.stageObjectInstances[i].position))
        {
            return false;
        }
    }
    return true;
}

void EncodeRngSection(BufferWriter &writer, const PortableRngState &rng)
{
    writer.WriteU16(rng.seed);
    writer.WriteU32(rng.generationCount);
}

bool DecodeRngSection(BufferReader &reader, PortableRngState &rng)
{
    return reader.ReadU16(rng.seed) && reader.ReadU32(rng.generationCount);
}

void EncodeCatkSection(BufferWriter &writer, const std::array<Catk, CATK_NUM_CAPTURES> &catk)
{
    writer.WriteU32((u32)catk.size());
    writer.bytes.insert(writer.bytes.end(), reinterpret_cast<const u8 *>(catk.data()),
                        reinterpret_cast<const u8 *>(catk.data()) + sizeof(Catk) * catk.size());
}

bool DecodeCatkSection(BufferReader &reader, std::array<Catk, CATK_NUM_CAPTURES> &catk)
{
    u32 count = 0;
    if (!reader.ReadU32(count) || count != (u32)catk.size() || !reader.CanRead(sizeof(Catk) * catk.size()))
    {
        return false;
    }

    std::memcpy(catk.data(), &reader.bytes[reader.cursor], sizeof(Catk) * catk.size());
    reader.cursor += sizeof(Catk) * catk.size();
    return true;
}

void EncodeStageRefsSection(BufferWriter &writer, const DgsStageRefs &refs)
{
    writer.WriteI32(refs.beginningOfScript.value);
    writer.WriteU32((u32)refs.objectOffsets.size());
    for (const auto &offset : refs.objectOffsets)
    {
        writer.WriteI32(offset.value);
    }
    writer.WriteU32((u32)refs.quadVmRefs.size());
    for (const auto &vmRefs : refs.quadVmRefs)
    {
        WriteAnmVmRefs(writer, vmRefs);
    }
    WriteAnmVmRefs(writer, refs.spellcardBackground);
    WriteAnmVmRefs(writer, refs.extraBackground);
}

bool DecodeStageRefsSection(BufferReader &reader, DgsStageRefs &refs)
{
    u32 count = 0;
    if (!reader.ReadI32(refs.beginningOfScript.value) || !reader.ReadU32(count))
    {
        return false;
    }
    refs.objectOffsets.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadI32(refs.objectOffsets[i].value))
        {
            return false;
        }
    }
    if (!reader.ReadU32(count))
    {
        return false;
    }
    refs.quadVmRefs.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!ReadAnmVmRefs(reader, refs.quadVmRefs[i]))
        {
            return false;
        }
    }
    return ReadAnmVmRefs(reader, refs.spellcardBackground) && ReadAnmVmRefs(reader, refs.extraBackground);
}

void EncodeEnemyRefsSection(BufferWriter &writer, const DgsEnemyManagerRefs &refs)
{
    for (const auto &bossIndex : refs.bossIndices)
    {
        writer.WriteI32(bossIndex.value);
    }
    writer.WriteI32(refs.timelineOffset.value);
    writer.WriteU32((u32)refs.enemyRefs.size());
    for (const auto &enemyRefs : refs.enemyRefs)
    {
        WriteAnmVmRefs(writer, enemyRefs.primaryVm);
        for (const auto &vmRefs : enemyRefs.vms)
        {
            WriteAnmVmRefs(writer, vmRefs);
        }
        WriteEnemyContextRefs(writer, enemyRefs.currentContext);
        for (const auto &ctxRefs : enemyRefs.savedContextStack)
        {
            WriteEnemyContextRefs(writer, ctxRefs);
        }
        for (const auto &laserIndex : enemyRefs.laserIndices)
        {
            writer.WriteI32(laserIndex.value);
        }
        for (const auto &effectIndex : enemyRefs.effectIndices)
        {
            writer.WriteI32(effectIndex.value);
        }
    }
}

bool DecodeEnemyRefsSection(BufferReader &reader, DgsEnemyManagerRefs &refs)
{
    for (auto &bossIndex : refs.bossIndices)
    {
        if (!reader.ReadI32(bossIndex.value))
        {
            return false;
        }
    }
    if (!reader.ReadI32(refs.timelineOffset.value))
    {
        return false;
    }

    u32 enemyCount = 0;
    if (!reader.ReadU32(enemyCount))
    {
        return false;
    }
    refs.enemyRefs.resize(enemyCount);
    for (u32 i = 0; i < enemyCount; ++i)
    {
        auto &enemyRefs = refs.enemyRefs[i];
        if (!ReadAnmVmRefs(reader, enemyRefs.primaryVm))
        {
            return false;
        }
        for (auto &vmRefs : enemyRefs.vms)
        {
            if (!ReadAnmVmRefs(reader, vmRefs))
            {
                return false;
            }
        }
        if (!ReadEnemyContextRefs(reader, enemyRefs.currentContext))
        {
            return false;
        }
        for (auto &ctxRefs : enemyRefs.savedContextStack)
        {
            if (!ReadEnemyContextRefs(reader, ctxRefs))
            {
                return false;
            }
        }
        for (auto &laserIndex : enemyRefs.laserIndices)
        {
            if (!reader.ReadI32(laserIndex.value))
            {
                return false;
            }
        }
        for (auto &effectIndex : enemyRefs.effectIndices)
        {
            if (!reader.ReadI32(effectIndex.value))
            {
                return false;
            }
        }
    }
    return true;
}

void EncodeEclRefsSection(BufferWriter &writer, const PortableEclScriptState &refs)
{
    writer.WriteU8(refs.hasEclFile ? 1 : 0);
    writer.WriteI32(refs.timelineOffset);
    writer.WriteU32((u32)refs.subTableEntryOffsets.size());
    for (i32 offset : refs.subTableEntryOffsets)
    {
        writer.WriteI32(offset);
    }
}

bool DecodeEclRefsSection(BufferReader &reader, PortableEclScriptState &refs)
{
    u8 hasEclFile = 0;
    u32 count = 0;
    if (!reader.ReadU8(hasEclFile) || !reader.ReadI32(refs.timelineOffset) || !reader.ReadU32(count))
    {
        return false;
    }
    refs.hasEclFile = hasEclFile != 0;
    refs.subTableEntryOffsets.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadI32(refs.subTableEntryOffsets[i]))
        {
            return false;
        }
    }
    return true;
}

void EncodeEclCoreSection(BufferWriter &writer, const PortableEclManagerCoreState &core)
{
    writer.WriteU8(core.hasEclFile ? 1 : 0);
    writer.WriteString(core.resource.resourcePath);
    writer.WriteU64(core.resource.resourceContentHash);
    writer.WriteU32(core.resource.resourceSizeBytes);
    writer.WriteI32(core.subCount);
    writer.WriteI32(core.mainCount);
    for (i32 offset : core.timelineOffsets)
    {
        writer.WriteI32(offset);
    }
    writer.WriteI32(core.activeTimelineSlot);
    writer.WriteI32(core.activeTimelineOffset);
    writer.WriteU32((u32)core.subTableEntryOffsets.size());
    for (i32 offset : core.subTableEntryOffsets)
    {
        writer.WriteI32(offset);
    }
}

bool DecodeEclCoreSection(BufferReader &reader, PortableEclManagerCoreState &core)
{
    u8 hasEclFile = 0;
    u32 count = 0;
    if (!reader.ReadU8(hasEclFile) || !reader.ReadString(core.resource.resourcePath) ||
        !reader.ReadU64(core.resource.resourceContentHash) || !reader.ReadU32(core.resource.resourceSizeBytes) ||
        !reader.ReadI32(core.subCount) || !reader.ReadI32(core.mainCount))
    {
        return false;
    }
    core.hasEclFile = hasEclFile != 0;
    for (i32 &offset : core.timelineOffsets)
    {
        if (!reader.ReadI32(offset))
        {
            return false;
        }
    }
    if (!reader.ReadI32(core.activeTimelineSlot) || !reader.ReadI32(core.activeTimelineOffset) ||
        !reader.ReadU32(count))
    {
        return false;
    }
    core.subTableEntryOffsets.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadI32(core.subTableEntryOffsets[i]))
        {
            return false;
        }
    }
    return true;
}

void EncodeStageCoreSection(BufferWriter &writer, const PortableStageCoreState &core)
{
    writer.WriteU8(core.hasStageData ? 1 : 0);
    writer.WriteString(core.resource.stdPath);
    writer.WriteU64(core.resource.stdContentHash);
    writer.WriteU32(core.resource.stdSizeBytes);
    writer.WriteString(core.resource.anmPath);
    writer.WriteU64(core.resource.anmContentHash);
    writer.WriteU32(core.resource.anmSizeBytes);
    writer.WriteU32(core.stage);
    writer.WriteI32(core.objectsCount);
    writer.WriteI32(core.quadCount);
    writer.WriteTimer(core.scriptTime);
    writer.WriteI32(core.instructionIndex);
    writer.WriteTimer(core.timer);
    writer.WriteVec3(core.position);
    writer.WriteF32(core.skyFog.nearPlane);
    writer.WriteF32(core.skyFog.farPlane);
    writer.WriteU32(core.skyFog.color);
    writer.WriteF32(core.skyFogInterpInitial.nearPlane);
    writer.WriteF32(core.skyFogInterpInitial.farPlane);
    writer.WriteU32(core.skyFogInterpInitial.color);
    writer.WriteF32(core.skyFogInterpFinal.nearPlane);
    writer.WriteF32(core.skyFogInterpFinal.farPlane);
    writer.WriteU32(core.skyFogInterpFinal.color);
    writer.WriteI32(core.skyFogInterpDuration);
    writer.WriteTimer(core.skyFogInterpTimer);
    writer.WriteU8(core.skyFogNeedsSetup);
    writer.WriteI32((i32)core.spellcardState);
    writer.WriteI32(core.ticksSinceSpellcardStarted);
    writer.WriteU8(core.unpauseFlag);
    writer.WriteVec3(core.facingDirInterpInitial);
    writer.WriteVec3(core.facingDirInterpFinal);
    writer.WriteI32(core.facingDirInterpDuration);
    writer.WriteTimer(core.facingDirInterpTimer);
    writer.WriteVec3(core.positionInterpFinal);
    writer.WriteI32(core.positionInterpEndTime);
    writer.WriteVec3(core.positionInterpInitial);
    writer.WriteI32(core.positionInterpStartTime);
    writer.WriteVec3(core.currentCameraFacingDir);
    writer.WriteU32((u32)core.objectFlags.size());
    for (u8 flags : core.objectFlags)
    {
        writer.WriteU8(flags);
    }
    writer.WriteU32((u32)core.quadVms.size());
    for (const auto &vm : core.quadVms)
    {
        WritePortableAnmVmState(writer, vm);
    }
    WritePortableAnmVmState(writer, core.spellcardBackground);
    WritePortableAnmVmState(writer, core.extraBackground);
}

bool DecodeStageCoreSection(BufferReader &reader, PortableStageCoreState &core)
{
    u8 hasStageData = 0;
    u8 skyFogNeedsSetup = 0;
    u8 unpauseFlag = 0;
    u32 count = 0;
    i32 spellcardState = 0;
    if (!reader.ReadU8(hasStageData) || !reader.ReadString(core.resource.stdPath) ||
        !reader.ReadU64(core.resource.stdContentHash) || !reader.ReadU32(core.resource.stdSizeBytes) ||
        !reader.ReadString(core.resource.anmPath) || !reader.ReadU64(core.resource.anmContentHash) ||
        !reader.ReadU32(core.resource.anmSizeBytes) || !reader.ReadU32(core.stage) ||
        !reader.ReadI32(core.objectsCount) || !reader.ReadI32(core.quadCount) || !reader.ReadTimer(core.scriptTime) ||
        !reader.ReadI32(core.instructionIndex) || !reader.ReadTimer(core.timer) || !reader.ReadVec3(core.position) ||
        !reader.ReadF32(core.skyFog.nearPlane) || !reader.ReadF32(core.skyFog.farPlane) ||
        !reader.ReadU32(core.skyFog.color) || !reader.ReadF32(core.skyFogInterpInitial.nearPlane) ||
        !reader.ReadF32(core.skyFogInterpInitial.farPlane) || !reader.ReadU32(core.skyFogInterpInitial.color) ||
        !reader.ReadF32(core.skyFogInterpFinal.nearPlane) || !reader.ReadF32(core.skyFogInterpFinal.farPlane) ||
        !reader.ReadU32(core.skyFogInterpFinal.color) || !reader.ReadI32(core.skyFogInterpDuration) ||
        !reader.ReadTimer(core.skyFogInterpTimer) || !reader.ReadU8(skyFogNeedsSetup) ||
        !reader.ReadI32(spellcardState) || !reader.ReadI32(core.ticksSinceSpellcardStarted) ||
        !reader.ReadU8(unpauseFlag) || !reader.ReadVec3(core.facingDirInterpInitial) ||
        !reader.ReadVec3(core.facingDirInterpFinal) || !reader.ReadI32(core.facingDirInterpDuration) ||
        !reader.ReadTimer(core.facingDirInterpTimer) || !reader.ReadVec3(core.positionInterpFinal) ||
        !reader.ReadI32(core.positionInterpEndTime) || !reader.ReadVec3(core.positionInterpInitial) ||
        !reader.ReadI32(core.positionInterpStartTime) || !reader.ReadVec3(core.currentCameraFacingDir) ||
        !reader.ReadU32(count))
    {
        return false;
    }
    core.hasStageData = hasStageData != 0;
    core.skyFogNeedsSetup = skyFogNeedsSetup;
    core.spellcardState = (SpellcardState)spellcardState;
    core.unpauseFlag = unpauseFlag;
    core.objectFlags.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadU8(core.objectFlags[i]))
        {
            return false;
        }
    }
    if (!reader.ReadU32(count))
    {
        return false;
    }
    core.quadVms.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!ReadPortableAnmVmState(reader, core.quadVms[i]))
        {
            return false;
        }
    }
    return ReadPortableAnmVmState(reader, core.spellcardBackground) &&
           ReadPortableAnmVmState(reader, core.extraBackground);
}

void EncodeShellSyncSection(BufferWriter &writer, const PortableShellSyncState &shellSync)
{
    writer.WriteI32(shellSync.bgmTrackIndex);
    writer.WriteU32((u32)shellSync.bossAssetProfile);
    writer.WriteU8(shellSync.hideStageNameIntro ? 1 : 0);
    writer.WriteU8(shellSync.hideSongNameIntro ? 1 : 0);
}

bool DecodeShellSyncSection(BufferReader &reader, PortableShellSyncState &shellSync)
{
    u32 bossAssetProfile = 0;
    u8 hideStageNameIntro = 0;
    u8 hideSongNameIntro = 0;
    if (!reader.ReadI32(shellSync.bgmTrackIndex) || !reader.ReadU32(bossAssetProfile) ||
        !reader.ReadU8(hideStageNameIntro) || !reader.ReadU8(hideSongNameIntro))
    {
        return false;
    }
    shellSync.bossAssetProfile = (PortableBossAssetProfile)bossAssetProfile;
    shellSync.hideStageNameIntro = hideStageNameIntro != 0;
    shellSync.hideSongNameIntro = hideSongNameIntro != 0;
    return true;
}

void EncodeGuiStateSection(BufferWriter &writer, const PortableGuiState &gui)
{
    writer.WriteU8(gui.hasGuiImpl ? 1 : 0);
    writer.WriteU8(gui.flag0);
    writer.WriteU8(gui.flag1);
    writer.WriteU8(gui.flag2);
    writer.WriteU8(gui.flag3);
    writer.WriteU8(gui.flag4);
    writer.WriteU8(gui.bossHealthBarState);
    writer.WriteU8(gui.bossPresent ? 1 : 0);
    writer.WriteU32(gui.bossUIOpacity);
    writer.WriteI32(gui.eclSetLives);
    writer.WriteI32(gui.spellcardSecondsRemaining);
    writer.WriteI32(gui.lastSpellcardSecondsRemaining);
    writer.WriteF32(gui.bossHealthBar1);
    writer.WriteF32(gui.bossHealthBar2);
    writer.WriteF32(gui.bombSpellcardBarLength);
    writer.WriteF32(gui.blueSpellcardBarLength);
    WritePortableAnmVmState(writer, gui.playerSpellcardPortrait);
    WritePortableAnmVmState(writer, gui.enemySpellcardPortrait);
    WritePortableAnmVmState(writer, gui.bombSpellcardName);
    WritePortableAnmVmState(writer, gui.enemySpellcardName);
    WritePortableAnmVmState(writer, gui.bombSpellcardBackground);
    WritePortableAnmVmState(writer, gui.enemySpellcardBackground);
    WritePortableAnmVmState(writer, gui.loadingScreenSprite);
    writer.WriteString(gui.bombSpellcardText);
    writer.WriteString(gui.enemySpellcardText);
}

bool DecodeGuiStateSection(BufferReader &reader, PortableGuiState &gui)
{
    u8 hasGuiImpl = 0;
    u8 bossPresent = 0;
    if (!reader.ReadU8(hasGuiImpl) || !reader.ReadU8(gui.flag0) || !reader.ReadU8(gui.flag1) ||
        !reader.ReadU8(gui.flag2) || !reader.ReadU8(gui.flag3) || !reader.ReadU8(gui.flag4) ||
        !reader.ReadU8(gui.bossHealthBarState) || !reader.ReadU8(bossPresent) || !reader.ReadU32(gui.bossUIOpacity) ||
        !reader.ReadI32(gui.eclSetLives) || !reader.ReadI32(gui.spellcardSecondsRemaining) ||
        !reader.ReadI32(gui.lastSpellcardSecondsRemaining) || !reader.ReadF32(gui.bossHealthBar1) ||
        !reader.ReadF32(gui.bossHealthBar2) || !reader.ReadF32(gui.bombSpellcardBarLength) ||
        !reader.ReadF32(gui.blueSpellcardBarLength) ||
        !ReadPortableAnmVmState(reader, gui.playerSpellcardPortrait) ||
        !ReadPortableAnmVmState(reader, gui.enemySpellcardPortrait) ||
        !ReadPortableAnmVmState(reader, gui.bombSpellcardName) ||
        !ReadPortableAnmVmState(reader, gui.enemySpellcardName) ||
        !ReadPortableAnmVmState(reader, gui.bombSpellcardBackground) ||
        !ReadPortableAnmVmState(reader, gui.enemySpellcardBackground) ||
        !ReadPortableAnmVmState(reader, gui.loadingScreenSprite))
    {
        return false;
    }

    gui.hasGuiImpl = hasGuiImpl != 0;
    gui.bossPresent = bossPresent != 0;

    gui.bombSpellcardText.clear();
    gui.enemySpellcardText.clear();
    if (reader.cursor < reader.bytes.size())
    {
        if (!reader.ReadString(gui.bombSpellcardText) || !reader.ReadString(gui.enemySpellcardText))
        {
            return false;
        }
    }

    return true;
}

void EncodeActorsSection(BufferWriter &writer, const PortableEnemyManagerState &actors)
{
    writer.WriteString(actors.stgEnmAnmFilename);
    writer.WriteString(actors.stgEnm2AnmFilename);
    WritePortableEnemyState(writer, actors.enemyTemplate);
    writer.WriteU32((u32)actors.enemies.size());
    for (const auto &enemy : actors.enemies)
    {
        WritePortableEnemyState(writer, enemy);
    }
    for (i32 bossIndex : actors.bossIndices)
    {
        writer.WriteI32(bossIndex);
    }
    writer.WriteU16(actors.randomItemSpawnIndex);
    writer.WriteU16(actors.randomItemTableIndex);
    writer.WriteI32(actors.enemyCount);
    WritePortableRunningSpellcard(writer, actors.spellcardInfo);
    writer.WriteI32(actors.unk_ee5d8);
    writer.WriteI32(actors.timelineOffset);
    writer.WriteTimer(actors.timelineTime);
}

bool DecodeActorsSection(BufferReader &reader, PortableEnemyManagerState &actors)
{
    if (!reader.ReadString(actors.stgEnmAnmFilename) || !reader.ReadString(actors.stgEnm2AnmFilename) ||
        !ReadPortableEnemyState(reader, actors.enemyTemplate))
    {
        return false;
    }

    u32 enemyCount = 0;
    if (!reader.ReadU32(enemyCount))
    {
        return false;
    }
    actors.enemies.resize(enemyCount);
    for (u32 i = 0; i < enemyCount; ++i)
    {
        if (!ReadPortableEnemyState(reader, actors.enemies[i]))
        {
            return false;
        }
    }
    for (i32 &bossIndex : actors.bossIndices)
    {
        if (!reader.ReadI32(bossIndex))
        {
            return false;
        }
    }
    return reader.ReadU16(actors.randomItemSpawnIndex) && reader.ReadU16(actors.randomItemTableIndex) &&
           reader.ReadI32(actors.enemyCount) && ReadPortableRunningSpellcard(reader, actors.spellcardInfo) &&
           reader.ReadI32(actors.unk_ee5d8) && reader.ReadI32(actors.timelineOffset) &&
           reader.ReadTimer(actors.timelineTime);
}

void EncodeActorsSection(BufferWriter &writer, const PortableGameplayState &state)
{
    EncodeActorsSection(writer, state.enemyActors);
    writer.WriteU32((u32)state.players.size());
    for (const auto &player : state.players)
    {
        WritePortablePlayerState(writer, player);
    }
    WritePortableBulletManagerState(writer, state.bulletActors);
    WritePortableItemManagerState(writer, state.itemActors);
    WritePortableEffectManagerState(writer, state.effectActors);
}

bool DecodeActorsSection(BufferReader &reader, PortableGameplayState &state, bool &hasItems, bool &hasEffects)
{
    hasItems = false;
    hasEffects = false;
    if (!DecodeActorsSection(reader, state.enemyActors))
    {
        return false;
    }

    u32 playerCount = 0;
    if (!reader.ReadU32(playerCount) || playerCount != state.players.size())
    {
        return false;
    }
    for (auto &player : state.players)
    {
        if (!ReadPortablePlayerState(reader, player))
        {
            return false;
        }
    }
    if (!ReadPortableBulletManagerState(reader, state.bulletActors))
    {
        return false;
    }
    if (reader.cursor == reader.bytes.size())
    {
        return true;
    }
    if (!ReadPortableItemManagerState(reader, state.itemActors))
    {
        return false;
    }
    hasItems = true;
    if (reader.cursor == reader.bytes.size())
    {
        return true;
    }
    if (!ReadPortableEffectManagerState(reader, state.effectActors))
    {
        return false;
    }
    hasEffects = true;
    return true;
}

void EncodeFingerprintsSection(BufferWriter &writer, const PortableGameplayShadowFingerprints &fingerprints)
{
    writer.WriteU64(fingerprints.dgs.gameManager);
    writer.WriteU64(fingerprints.dgs.player1);
    writer.WriteU64(fingerprints.dgs.player2);
    writer.WriteU64(fingerprints.dgs.bulletManager);
    writer.WriteU64(fingerprints.dgs.enemyManager);
    writer.WriteU64(fingerprints.dgs.itemManager);
    writer.WriteU64(fingerprints.dgs.effectManager);
    writer.WriteU64(fingerprints.dgs.stageState);
    writer.WriteU64(fingerprints.dgs.eclManager);
    writer.WriteU64(fingerprints.dgs.rng);
    writer.WriteU64(fingerprints.dgs.runtime);
    writer.WriteU64(fingerprints.dgs.refs);
    writer.WriteU64(fingerprints.dgs.unresolved);
    writer.WriteU64(fingerprints.dgs.combined);
}

bool DecodeFingerprintsSection(BufferReader &reader, PortableGameplayShadowFingerprints &fingerprints)
{
    return reader.ReadU64(fingerprints.dgs.gameManager) && reader.ReadU64(fingerprints.dgs.player1) &&
           reader.ReadU64(fingerprints.dgs.player2) && reader.ReadU64(fingerprints.dgs.bulletManager) &&
           reader.ReadU64(fingerprints.dgs.enemyManager) && reader.ReadU64(fingerprints.dgs.itemManager) &&
           reader.ReadU64(fingerprints.dgs.effectManager) && reader.ReadU64(fingerprints.dgs.stageState) &&
           reader.ReadU64(fingerprints.dgs.eclManager) && reader.ReadU64(fingerprints.dgs.rng) &&
           reader.ReadU64(fingerprints.dgs.runtime) && reader.ReadU64(fingerprints.dgs.refs) &&
           reader.ReadU64(fingerprints.dgs.unresolved) && reader.ReadU64(fingerprints.dgs.combined);
}

void EncodeAuditSection(BufferWriter &writer, const PortableGameplayState &state)
{
    writer.WriteU32((u32)state.mustNormalizeBeforeRestore.size());
    for (const auto &entry : state.mustNormalizeBeforeRestore)
    {
        writer.WriteString(entry);
    }
    writer.WriteU32((u32)state.excludedByDesign.size());
    for (const auto &entry : state.excludedByDesign)
    {
        writer.WriteString(entry);
    }
}

bool DecodeAuditSection(BufferReader &reader, PortableGameplayState &state)
{
    u32 count = 0;
    if (!reader.ReadU32(count))
    {
        return false;
    }
    state.mustNormalizeBeforeRestore.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadString(state.mustNormalizeBeforeRestore[i]))
        {
            return false;
        }
    }

    if (!reader.ReadU32(count))
    {
        return false;
    }
    state.excludedByDesign.resize(count);
    for (u32 i = 0; i < count; ++i)
    {
        if (!reader.ReadString(state.excludedByDesign[i]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

std::vector<u8> EncodePortableGameplayState(const PortableGameplayState &state)
{
    BufferWriter root;
    BufferWriter catalog;
    BufferWriter core;
    BufferWriter runtime;
    BufferWriter stageRefs;
    BufferWriter enemyRefs;
    BufferWriter eclRefs;
    BufferWriter eclCore;
    BufferWriter stageCore;
    BufferWriter shellSync;
    BufferWriter gui;
    BufferWriter rngState;
    BufferWriter catkState;
    BufferWriter actors;
    BufferWriter fingerprints;
    BufferWriter audit;

    EncodeCatalogSection(catalog, state.catalog);
    EncodeCoreSection(core, state.core);
    EncodeRuntimeSection(runtime, state.runtime);
    EncodeStageRefsSection(stageRefs, state.stageRefs);
    EncodeEnemyRefsSection(enemyRefs, state.enemyRefs);
    EncodeEclRefsSection(eclRefs, state.eclScripts);
    EncodeEclCoreSection(eclCore, state.eclCore);
    EncodeStageCoreSection(stageCore, state.stageCore);
    EncodeShellSyncSection(shellSync, state.shellSync);
    EncodeGuiStateSection(gui, state.gui);
    EncodeRngSection(rngState, state.rng);
    EncodeCatkSection(catkState, state.catk);
    EncodeActorsSection(actors, state);
    EncodeFingerprintsSection(fingerprints, state.shadowFingerprints);
    EncodeAuditSection(audit, state);

    PortableGameplayEnvelopeHeader header = state.header;
    header.magic = kPortableGameplayMagic;
    const bool writeV6 = header.version >= (u32)PortableGameplaySchemaVersion::V6 ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitRng) ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitCatk);
    const bool writeV5 = writeV6 || header.version >= (u32)PortableGameplaySchemaVersion::V5 ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitGui);
    const bool writeV4 = writeV5 || header.version >= (u32)PortableGameplaySchemaVersion::V4 ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasShellSyncHints);
    const bool writeV3 = header.version >= (u32)PortableGameplaySchemaVersion::V3 ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitStageCore);
    const bool writeV2 = writeV3 || header.version >= (u32)PortableGameplaySchemaVersion::V2 ||
                         HasPortableCaptureFlag(state.captureFlags, PortableCaptureFlag_HasExplicitEclCore);
    header.version = writeV6 ? (u32)PortableGameplaySchemaVersion::V6
                             : (writeV5 ? (u32)PortableGameplaySchemaVersion::V5
                             : (writeV4 ? (u32)PortableGameplaySchemaVersion::V4
                             : (writeV3 ? (u32)PortableGameplaySchemaVersion::V3
                             : (writeV2 ? (u32)PortableGameplaySchemaVersion::V2
                                        : (u32)PortableGameplaySchemaVersion::V1))));
    header.sectionCount = writeV6 ? 15u : (writeV5 ? 13u : (writeV4 ? 12u : (writeV3 ? 11u : (writeV2 ? 10u : 9u))));

    root.WriteU32(header.magic);
    root.WriteU32(header.version);
    root.WriteU32(header.sectionCount);
    root.WriteU32(0);

    WriteSectionHeader(root, PortableGameplaySectionId::ResourceCatalog, catalog.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::Core, core.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::Runtime, runtime.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::StageRefs, stageRefs.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::EnemyRefs, enemyRefs.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::EclRefs, eclRefs.bytes);
    if (writeV2)
    {
        WriteSectionHeader(root, PortableGameplaySectionId::EclCore, eclCore.bytes);
    }
    if (writeV3)
    {
        WriteSectionHeader(root, PortableGameplaySectionId::StageCore, stageCore.bytes);
    }
    if (writeV4)
    {
        WriteSectionHeader(root, PortableGameplaySectionId::ShellSync, shellSync.bytes);
    }
    if (writeV5)
    {
        WriteSectionHeader(root, PortableGameplaySectionId::GuiState, gui.bytes);
    }
    if (writeV6)
    {
        WriteSectionHeader(root, PortableGameplaySectionId::RngState, rngState.bytes);
        WriteSectionHeader(root, PortableGameplaySectionId::CatkState, catkState.bytes);
    }
    WriteSectionHeader(root, PortableGameplaySectionId::Actors, actors.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::Fingerprints, fingerprints.bytes);
    WriteSectionHeader(root, PortableGameplaySectionId::Audit, audit.bytes);

    const u32 totalBytes = (u32)root.bytes.size();
    for (int shift = 0; shift < 32; shift += 8)
    {
        root.bytes[12 + shift / 8] = (u8)((totalBytes >> shift) & 0xff);
    }
    return root.bytes;
}

bool DecodePortableGameplayState(const std::vector<u8> &bytes, PortableGameplayState &state)
{
    state.header = {};
    state.captureFlags = PortableCaptureFlag_None;
    state.mustNormalizeBeforeRestore.clear();
    state.excludedByDesign.clear();
    state.runtime.stageObjectInstances.clear();
    state.eclScripts.subTableEntryOffsets.clear();
    state.eclCore.resource.resourcePath.clear();
    state.eclCore.resource.resourceContentHash = 0;
    state.eclCore.resource.resourceSizeBytes = 0;
    state.eclCore.subCount = 0;
    state.eclCore.mainCount = 0;
    state.eclCore.timelineOffsets = {{-1, -1, -1}};
    state.eclCore.activeTimelineSlot = -1;
    state.eclCore.activeTimelineOffset = -1;
    state.eclCore.subTableEntryOffsets.clear();
    state.stageCore.hasStageData = false;
    state.stageCore.resource.stdPath.clear();
    state.stageCore.resource.stdContentHash = 0;
    state.stageCore.resource.stdSizeBytes = 0;
    state.stageCore.resource.anmPath.clear();
    state.stageCore.resource.anmContentHash = 0;
    state.stageCore.resource.anmSizeBytes = 0;
    state.stageCore.objectFlags.clear();
    state.stageCore.quadVms.clear();
    state.shellSync = {};
    state.gui = {};
    state.rng = {};
    std::memset(state.catk.data(), 0, sizeof(Catk) * state.catk.size());
    state.enemyActors.stgEnmAnmFilename.clear();
    state.enemyActors.stgEnm2AnmFilename.clear();
    state.enemyActors.enemies.clear();
    BufferReader root{bytes};
    if (!root.ReadU32(state.header.magic) || !root.ReadU32(state.header.version) ||
        !root.ReadU32(state.header.sectionCount) || !root.ReadU32(state.header.totalBytes))
    {
        return false;
    }

    if (state.header.magic != kPortableGameplayMagic)
    {
        return false;
    }

    for (u32 sectionIndex = 0; sectionIndex < state.header.sectionCount; ++sectionIndex)
    {
        PortableGameplaySectionHeader sectionHeader;
        if (!root.ReadU16(sectionHeader.id) || !root.ReadU16(sectionHeader.version) ||
            !root.ReadU32(sectionHeader.sizeBytes) || !root.CanRead(sectionHeader.sizeBytes))
        {
            return false;
        }

        std::vector<u8> payload(sectionHeader.sizeBytes);
        for (u32 i = 0; i < sectionHeader.sizeBytes; ++i)
        {
            payload[i] = bytes[root.cursor++];
        }
        BufferReader sectionReader{payload};

        switch ((PortableGameplaySectionId)sectionHeader.id)
        {
        case PortableGameplaySectionId::ResourceCatalog:
            if (!DecodeCatalogSection(sectionReader, state.catalog))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasResourceCatalog;
            break;
        case PortableGameplaySectionId::Core:
            if (!DecodeCoreSection(sectionReader, state.core))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitCore;
            break;
        case PortableGameplaySectionId::Runtime:
            if (!DecodeRuntimeSection(sectionReader, state.runtime))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitRuntime;
            break;
        case PortableGameplaySectionId::StageRefs:
            if (!DecodeStageRefsSection(sectionReader, state.stageRefs))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasStageRefs;
            break;
        case PortableGameplaySectionId::EnemyRefs:
            if (!DecodeEnemyRefsSection(sectionReader, state.enemyRefs))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasEnemyRefs;
            break;
        case PortableGameplaySectionId::EclRefs:
            if (!DecodeEclRefsSection(sectionReader, state.eclScripts))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasEclRefs;
            break;
        case PortableGameplaySectionId::EclCore:
            if (!DecodeEclCoreSection(sectionReader, state.eclCore))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitEclCore;
            break;
        case PortableGameplaySectionId::StageCore:
            if (!DecodeStageCoreSection(sectionReader, state.stageCore))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitStageCore;
            break;
        case PortableGameplaySectionId::ShellSync:
            if (!DecodeShellSyncSection(sectionReader, state.shellSync))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasShellSyncHints;
            break;
        case PortableGameplaySectionId::GuiState:
            if (!DecodeGuiStateSection(sectionReader, state.gui))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitGui;
            break;
        case PortableGameplaySectionId::RngState:
            if (!DecodeRngSection(sectionReader, state.rng))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitRng;
            break;
        case PortableGameplaySectionId::CatkState:
            if (!DecodeCatkSection(sectionReader, state.catk))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitCatk;
            break;
        case PortableGameplaySectionId::Actors:
        {
            bool hasItems = false;
            bool hasEffects = false;
            if (!DecodeActorsSection(sectionReader, state, hasItems, hasEffects))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasExplicitActors;
            state.captureFlags |= PortableCaptureFlag_HasIndependentRestore;
            state.captureFlags |= PortableCaptureFlag_HasExplicitPlayers;
            state.captureFlags |= PortableCaptureFlag_HasExplicitBullets;
            if (hasItems)
            {
                state.captureFlags |= PortableCaptureFlag_HasExplicitItems;
            }
            if (hasEffects)
            {
                state.captureFlags |= PortableCaptureFlag_HasExplicitEffects;
            }
            break;
        }
        case PortableGameplaySectionId::Fingerprints:
            if (!DecodeFingerprintsSection(sectionReader, state.shadowFingerprints))
            {
                return false;
            }
            state.captureFlags |= PortableCaptureFlag_HasShadowFingerprints;
            break;
        case PortableGameplaySectionId::Audit:
            if (!DecodeAuditSection(sectionReader, state))
            {
                return false;
            }
            break;
        default:
            return false;
        }

        if (sectionReader.cursor != payload.size())
        {
            return false;
        }
    }

    return true;
}

bool RoundTripPortableGameplayStateBinary(const PortableGameplayState &state, uint64_t *outBefore, uint64_t *outAfter)
{
    const uint64_t before = FingerprintPortableGameplayState(state);
    if (outBefore != nullptr)
    {
        *outBefore = before;
    }

    const std::vector<u8> bytes = EncodePortableGameplayState(state);
    auto decoded = std::make_unique<PortableGameplayState>();
    if (!DecodePortableGameplayState(bytes, *decoded))
    {
        if (outAfter != nullptr)
        {
            *outAfter = 0;
        }
        return false;
    }

    const uint64_t after = FingerprintPortableGameplayState(*decoded);
    if (outAfter != nullptr)
    {
        *outAfter = after;
    }
    return before == after;
}
} // namespace th06::DGS
