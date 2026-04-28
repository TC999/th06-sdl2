#include "AnmManager.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "IRenderer.hpp"
#include "Rng.hpp"
#include "Supervisor.hpp"
#include "TextHelper.hpp"
#include "ZunMath.hpp"
#include "i18n.hpp"
#include "utils.hpp"

// Diagnostic logging for point item texture bug investigation
#include <stdio.h>
#include "thprac_th06.h"
// Gate diagnostic output on the user-selected log level (Developer tab → Log Level).
// Warn macros emit at level >= 2 (Warn); Info macros emit at level >= 3 (Info).
// Diagnostic logs are firehose-class (per-sprite per-frame). Default level
// Warn(2) MUST stay quiet, otherwise diag_log.txt fills in seconds.
//   - DIAG (per-sprite draw)         → Verbose(5) only
//   - DIAG_INFO (per-sprite state)   → Debug(4)+
#define TH06_DIAG_LEVEL_OK_WARN() (THPrac::TH06::THPracGetLogLevel() >= 5)
#define TH06_DIAG_LEVEL_OK_INFO() (THPrac::TH06::THPracGetLogLevel() >= 4)
#ifdef __ANDROID__
#include <android/log.h>
#include <GLES2/gl2.h>
#define DIAG_LOG(fmt, ...) do { if (TH06_DIAG_LEVEL_OK_WARN()) __android_log_print(ANDROID_LOG_WARN, "TH06_DIAG", fmt, ##__VA_ARGS__); } while(0)
#define DIAG_LOG_INFO(fmt, ...) do { if (TH06_DIAG_LEVEL_OK_INFO()) __android_log_print(ANDROID_LOG_INFO, "TH06_DIAG", fmt, ##__VA_ARGS__); } while(0)
#else
#include <SDL_opengl.h>
FILE* _diag_get_file() {
    static FILE* f = nullptr;
    if (!f) { f = fopen("diag_log.txt", "w"); }
    return f;
}
#define DIAG_LOG(fmt, ...) do { if (TH06_DIAG_LEVEL_OK_WARN()) { FILE* _f = _diag_get_file(); if(_f) { fprintf(_f, "[TH06_DIAG] " fmt "\n", ##__VA_ARGS__); fflush(_f); } } } while(0)
#define DIAG_LOG_INFO(fmt, ...) do { if (TH06_DIAG_LEVEL_OK_INFO()) { FILE* _f = _diag_get_file(); if(_f) { fprintf(_f, "[TH06_DIAG_INFO] " fmt "\n", ##__VA_ARGS__); fflush(_f); } } } while(0)
#endif

#include <cmath>
#include <SDL_image.h>

#include <SDL.h>

namespace th06
{
DIFFABLE_STATIC(VertexTex1Xyzrwh, g_PrimitivesToDrawVertexBuf[4]);
DIFFABLE_STATIC(VertexTex1DiffuseXyzrwh, g_PrimitivesToDrawNoVertexBuf[4]);
DIFFABLE_STATIC(VertexTex1DiffuseXyz, g_PrimitivesToDrawUnknown[4]);
DIFFABLE_STATIC(AnmManager *, g_AnmManager)

u32 g_TextureFormatD3D8Mapping[6] = {0, 0, 0, 0, 0, 0};

#define TEX_FMT_UNKNOWN 0
#define TEX_FMT_A8R8G8B8 1
#define TEX_FMT_A1R5G5B5 2
#define TEX_FMT_R5G6B5 3
#define TEX_FMT_R8G8B8 4
#define TEX_FMT_A4R4G4B4 5

void AnmManager::ReleaseSurfaces(void)
{
    for (i32 idx = 0; idx < ARRAY_SIZE_SIGNED(this->surfaces); idx++)
    {
        if (this->surfaces[idx] != 0)
        {
            g_Renderer->DeleteTexture(this->surfaces[idx]);
            this->surfaces[idx] = 0;
        }
    }
}

void AnmManager::TakeScreenshotIfRequested()
{
    if (this->screenshotTextureId >= 0)
    {
        this->TakeScreenshot(this->screenshotTextureId, this->screenshotLeft, this->screenshotTop,
                             this->screenshotWidth, this->screenshotHeight);
        this->screenshotTextureId = -1;
    }
    return;
}

AnmManager::AnmManager()
{
    this->maybeLoadedSpriteCount = 0;

    memset(this, 0, sizeof(AnmManager));

    for (i32 spriteIndex = 0; spriteIndex < ARRAY_SIZE_SIGNED(this->sprites); spriteIndex++)
    {
        this->sprites[spriteIndex].sourceFileIndex = -1;
    }

    g_PrimitivesToDrawVertexBuf[3].position.w = 1.0;
    g_PrimitivesToDrawVertexBuf[2].position.w = g_PrimitivesToDrawVertexBuf[3].position.w;
    g_PrimitivesToDrawVertexBuf[1].position.w = g_PrimitivesToDrawVertexBuf[2].position.w;
    g_PrimitivesToDrawVertexBuf[0].position.w = g_PrimitivesToDrawVertexBuf[1].position.w;
    g_PrimitivesToDrawVertexBuf[0].textureUV.x = 0.0;
    g_PrimitivesToDrawVertexBuf[0].textureUV.y = 0.0;
    g_PrimitivesToDrawVertexBuf[1].textureUV.x = 1.0;
    g_PrimitivesToDrawVertexBuf[1].textureUV.y = 0.0;
    g_PrimitivesToDrawVertexBuf[2].textureUV.x = 0.0;
    g_PrimitivesToDrawVertexBuf[2].textureUV.y = 1.0;
    g_PrimitivesToDrawVertexBuf[3].textureUV.x = 1.0;
    g_PrimitivesToDrawVertexBuf[3].textureUV.y = 1.0;

    g_PrimitivesToDrawNoVertexBuf[3].position.w = 1.0;
    g_PrimitivesToDrawNoVertexBuf[2].position.w = g_PrimitivesToDrawNoVertexBuf[3].position.w;
    g_PrimitivesToDrawNoVertexBuf[1].position.w = g_PrimitivesToDrawNoVertexBuf[2].position.w;
    g_PrimitivesToDrawNoVertexBuf[0].position.w = g_PrimitivesToDrawNoVertexBuf[1].position.w;
    g_PrimitivesToDrawNoVertexBuf[0].textureUV.x = 0.0;
    g_PrimitivesToDrawNoVertexBuf[0].textureUV.y = 0.0;
    g_PrimitivesToDrawNoVertexBuf[1].textureUV.x = 1.0;
    g_PrimitivesToDrawNoVertexBuf[1].textureUV.y = 0.0;
    g_PrimitivesToDrawNoVertexBuf[2].textureUV.x = 0.0;
    g_PrimitivesToDrawNoVertexBuf[2].textureUV.y = 1.0;
    g_PrimitivesToDrawNoVertexBuf[3].textureUV.x = 1.0;
    g_PrimitivesToDrawNoVertexBuf[3].textureUV.y = 1.0;

    this->vertexBuffer = 0;
    this->currentTexture = 0;
    this->currentBlendMode = 0;
    this->currentColorOp = 0;
    this->currentTextureFactor = 1;
    this->currentVertexShader = 0;
    this->currentZWriteDisable = 0;
    this->screenshotTextureId = -1;
}

void AnmManager::SetupVertexBuffer()
{
    this->vertexBufferContents[2].position.x = -128;
    this->vertexBufferContents[0].position.x = -128;
    this->vertexBufferContents[3].position.x = 128;
    this->vertexBufferContents[1].position.x = 128;

    this->vertexBufferContents[1].position.y = -128;
    this->vertexBufferContents[0].position.y = -128;
    this->vertexBufferContents[3].position.y = 128;
    this->vertexBufferContents[2].position.y = 128;

    this->vertexBufferContents[3].position.z = 0;
    this->vertexBufferContents[2].position.z = 0;
    this->vertexBufferContents[1].position.z = 0;
    this->vertexBufferContents[0].position.z = 0;

    this->vertexBufferContents[2].textureUV.x = 0;
    this->vertexBufferContents[0].textureUV.x = 0;
    this->vertexBufferContents[3].textureUV.x = 1;
    this->vertexBufferContents[1].textureUV.x = 1;
    this->vertexBufferContents[1].textureUV.y = 0;
    this->vertexBufferContents[0].textureUV.y = 0;
    this->vertexBufferContents[3].textureUV.y = 1;
    this->vertexBufferContents[2].textureUV.y = 1;

    g_PrimitivesToDrawUnknown[0].position = this->vertexBufferContents[0].position;
    g_PrimitivesToDrawUnknown[1].position = this->vertexBufferContents[1].position;
    g_PrimitivesToDrawUnknown[2].position = this->vertexBufferContents[2].position;
    g_PrimitivesToDrawUnknown[3].position = this->vertexBufferContents[3].position;

    g_PrimitivesToDrawUnknown[0].textureUV.x = this->vertexBufferContents[0].textureUV.x;
    g_PrimitivesToDrawUnknown[0].textureUV.y = this->vertexBufferContents[0].textureUV.y;
    g_PrimitivesToDrawUnknown[1].textureUV.x = this->vertexBufferContents[1].textureUV.x;
    g_PrimitivesToDrawUnknown[1].textureUV.y = this->vertexBufferContents[1].textureUV.y;
    g_PrimitivesToDrawUnknown[2].textureUV.x = this->vertexBufferContents[2].textureUV.x;
    g_PrimitivesToDrawUnknown[2].textureUV.y = this->vertexBufferContents[2].textureUV.y;
    g_PrimitivesToDrawUnknown[3].textureUV.x = this->vertexBufferContents[3].textureUV.x;
    g_PrimitivesToDrawUnknown[3].textureUV.y = this->vertexBufferContents[3].textureUV.y;

    this->vertexBuffer = 1;
}

ZunResult AnmManager::LoadTexture(i32 textureIdx, char *textureName, i32 textureFormat, D3DCOLOR colorKey)
{
    ReleaseTexture(textureIdx);
    this->imageDataArray[textureIdx] = FileSystem::OpenPath(textureName, 0);

    if (this->imageDataArray[textureIdx] == NULL)
    {
        return ZUN_ERROR;
    }

    this->textures[textureIdx] = g_Renderer->CreateTextureFromMemory((const u8 *)this->imageDataArray[textureIdx], g_LastFileSize, colorKey, NULL, NULL);
    if (this->textures[textureIdx] == 0)
    {
        return ZUN_ERROR;
    }

    return ZUN_SUCCESS;
}

ZunResult AnmManager::LoadTextureAlphaChannel(i32 textureIdx, char *textureName, i32 textureFormat, D3DCOLOR colorKey)
{
    u8 *data = FileSystem::OpenPath(textureName, 0);

    if (data == NULL)
    {
        return ZUN_ERROR;
    }

    if (this->textures[textureIdx] == 0)
    {
        free(data);
        return ZUN_ERROR;
    }

    g_Renderer->CopyAlphaChannel(this->textures[textureIdx], data, g_LastFileSize, 0, 0);

    free(data);
    return ZUN_SUCCESS;
}

ZunResult AnmManager::CreateEmptyTexture(i32 textureIdx, u32 width, u32 height, i32 textureFormat)
{
    this->textures[textureIdx] = g_Renderer->CreateEmptyTexture(width, height);
    return ZUN_SUCCESS;
}

#pragma var_order(anm, anmName, rawSprite, index, curSpriteOffset, loadedSprite)
ZunResult AnmManager::LoadAnm(i32 anmIdx, char *path, i32 spriteIdxOffset)
{
    this->ReleaseAnm(anmIdx);
    this->anmFiles[anmIdx] = (AnmRawEntry *)FileSystem::OpenPath(path, 0);

    AnmRawEntry *anm = this->anmFiles[anmIdx];

    if (anm == NULL)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_SPRITE_CORRUPTED, path);
        return ZUN_ERROR;
    }

    anm->textureIdx = anmIdx;

    char *anmName = (char *)((u8 *)anm + anm->nameOffset);
    bool alphaAlreadyMerged = false;

    if (anmIdx == 6 || anmIdx == 4) // etama3 or stagebg
    {
        DIAG_LOG_INFO(
            "LoadAnm idx=%d path=%s texName=%s numSprites=%d numScripts=%d "
            "texW=%d texH=%d fmt=%u colorKey=0x%08X mipmapOff=%u",
            anmIdx, path, anmName, anm->numSprites, anm->numScripts,
            anm->width, anm->height, anm->format, anm->colorKey,
            anm->mipmapNameOffset);
    }

    if (*anmName == '@')
    {
        this->CreateEmptyTexture(anm->textureIdx, anm->width, anm->height, anm->format);
    }
    else if (anm->mipmapNameOffset != 0 && IsUsingGLES())
    {
        // GLES path: merge alpha on CPU before uploading to avoid fragile FBO readback.

        // Load and decode main texture image
        this->imageDataArray[anm->textureIdx] = FileSystem::OpenPath(anmName, 0);
        if (this->imageDataArray[anm->textureIdx] == NULL)
        {
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, anmName);
            return ZUN_ERROR;
        }
        i32 mainSize = (i32)g_LastFileSize;

        SDL_RWops *rw1 = SDL_RWFromConstMem(this->imageDataArray[anm->textureIdx], mainSize);
        SDL_Surface *mainSurf = rw1 ? IMG_Load_RW(rw1, 1) : NULL;
        if (!mainSurf)
        {
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, anmName);
            return ZUN_ERROR;
        }
        SDL_Surface *mainRgba = SDL_ConvertSurfaceFormat(mainSurf, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(mainSurf);
        if (!mainRgba)
        {
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, anmName);
            return ZUN_ERROR;
        }

        // Apply colorKey transparency
        if (anm->colorKey != 0)
        {
            u8 ckR = D3DCOLOR_R(anm->colorKey);
            u8 ckG = D3DCOLOR_G(anm->colorKey);
            u8 ckB = D3DCOLOR_B(anm->colorKey);
            SDL_LockSurface(mainRgba);
            u8 *pixels = (u8 *)mainRgba->pixels;
            for (i32 i = 0; i < mainRgba->w * mainRgba->h; i++)
            {
                u8 *p = pixels + i * 4;
                if (p[0] == ckR && p[1] == ckG && p[2] == ckB)
                    p[3] = 0;
                else
                    p[3] = 255; // D3D8 forces non-key pixels to fully opaque
            }
            SDL_UnlockSurface(mainRgba);
        }

        // Load and decode alpha channel image
        char *alphaName = (char *)((u8 *)anm + anm->mipmapNameOffset);
        u8 *alphaData = FileSystem::OpenPath(alphaName, 0);
        if (alphaData == NULL)
        {
            SDL_FreeSurface(mainRgba);
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, alphaName);
            return ZUN_ERROR;
        }
        i32 alphaSize = (i32)g_LastFileSize;
        SDL_RWops *rw2 = SDL_RWFromConstMem(alphaData, alphaSize);
        SDL_Surface *alphaSurf = rw2 ? IMG_Load_RW(rw2, 1) : NULL;
        if (!alphaSurf)
        {
            free(alphaData);
            SDL_FreeSurface(mainRgba);
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, alphaName);
            return ZUN_ERROR;
        }
        SDL_Surface *alphaRgba = SDL_ConvertSurfaceFormat(alphaSurf, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(alphaSurf);

        // Merge: copy blue channel of alpha image as alpha channel of main image.
        if (alphaRgba)
        {
            i32 copyW = alphaRgba->w < mainRgba->w ? alphaRgba->w : mainRgba->w;
            i32 copyH = alphaRgba->h < mainRgba->h ? alphaRgba->h : mainRgba->h;
            SDL_LockSurface(mainRgba);
            SDL_LockSurface(alphaRgba);

            for (i32 y = 0; y < copyH; y++)
            {
                u8 *src = (u8 *)alphaRgba->pixels + y * alphaRgba->pitch;
                u8 *dst = (u8 *)mainRgba->pixels + y * mainRgba->pitch;
                for (i32 x = 0; x < copyW; x++)
                    dst[x * 4 + 3] = src[x * 4 + 0]; // blue → alpha
            }
            SDL_UnlockSurface(alphaRgba);
            SDL_UnlockSurface(mainRgba);
            SDL_FreeSurface(alphaRgba);
        }
        free(alphaData);

        // Upload merged texture directly — no FBO readback needed
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mainRgba->w, mainRgba->h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, mainRgba->pixels);

        // Restore the previously-bound texture, matching the behaviour of
        // CreateTextureFromMemory / CreateEmptyTexture, so the renderer's
        // currentTexture cache stays in sync with the actual GL binding.
        u32 prevTex = g_Renderer->currentTexture;
        if (prevTex != 0 && prevTex != (u32)-1)
            glBindTexture(GL_TEXTURE_2D, prevTex);
        else
            glBindTexture(GL_TEXTURE_2D, 0);

        SDL_FreeSurface(mainRgba);

        this->textures[anm->textureIdx] = (u32)tex;
        alphaAlreadyMerged = true;
    }
    else if (this->LoadTexture(anm->textureIdx, anmName, anm->format, anm->colorKey) != ZUN_SUCCESS)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, anmName);
        return ZUN_ERROR;
    }

    if (anmIdx == 6 || anmIdx == 4) // etama3 or stagebg
    {
        DIAG_LOG_INFO(
            "LoadAnm idx=%d texture loaded: textures[%u]=%u alphaAlreadyMerged=%d",
            anmIdx, anm->textureIdx, this->textures[anm->textureIdx], (int)alphaAlreadyMerged);
    }

    if (anm->mipmapNameOffset != 0 && !alphaAlreadyMerged)
    {
        anmName = (char *)((u8 *)anm + anm->mipmapNameOffset);
        if (this->LoadTextureAlphaChannel(anm->textureIdx, anmName, anm->format, anm->colorKey) != ZUN_SUCCESS)
        {
            GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED, anmName);
            return ZUN_ERROR;
        }
    }

    anm->spriteIdxOffset = spriteIdxOffset;

    u32 *curSpriteOffset = anm->spriteOffsets;

    i32 index;
    AnmRawSprite rawSpriteAligned;

    for (index = 0; index < this->anmFiles[anmIdx]->numSprites; index++, curSpriteOffset++)
    {
        memcpy(&rawSpriteAligned, (u8 *)anm + *curSpriteOffset, sizeof(AnmRawSprite));

        AnmLoadedSprite loadedSprite;
        loadedSprite.sourceFileIndex = this->anmFiles[anmIdx]->textureIdx;
        loadedSprite.startPixelInclusive.x = rawSpriteAligned.offset.x;
        loadedSprite.startPixelInclusive.y = rawSpriteAligned.offset.y;
        loadedSprite.endPixelInclusive.x = rawSpriteAligned.offset.x + rawSpriteAligned.size.x;
        loadedSprite.endPixelInclusive.y = rawSpriteAligned.offset.y + rawSpriteAligned.size.y;
        loadedSprite.textureWidth = (float)anm->width;
        loadedSprite.textureHeight = (float)anm->height;
        this->LoadSprite(rawSpriteAligned.id + spriteIdxOffset, &loadedSprite);

        // Log sprite data for etama3 (anmIdx==6) — items
        if (anmIdx == 6)
        {
            DIAG_LOG_INFO(
                "LoadAnm[%d] sprite raw_id=%u global_id=%u srcFileIdx=%d "
                "px=[%.0f,%.0f]-[%.0f,%.0f] texSize=%dx%d",
                anmIdx, rawSpriteAligned.id, rawSpriteAligned.id + spriteIdxOffset,
                loadedSprite.sourceFileIndex,
                rawSpriteAligned.offset.x, rawSpriteAligned.offset.y,
                rawSpriteAligned.offset.x + rawSpriteAligned.size.x,
                rawSpriteAligned.offset.y + rawSpriteAligned.size.y,
                anm->width, anm->height);
        }
    }

    for (index = 0; index < anm->numScripts; index++, curSpriteOffset += 2)
    {
        this->scripts[curSpriteOffset[0] + spriteIdxOffset] = (AnmRawInstr *)((u8 *)anm + curSpriteOffset[1]);
        this->spriteIndices[curSpriteOffset[0] + spriteIdxOffset] = spriteIdxOffset;

        // Log script loading for etama3
        if (anmIdx == 6)
        {
            u32 globalIdx = curSpriteOffset[0] + spriteIdxOffset;
            AnmRawInstr *scriptPtr = this->scripts[globalIdx];
            AnmRawInstr *rawInstrPtr = (AnmRawInstr *)((u8 *)anm + curSpriteOffset[1]);
            DIAG_LOG_INFO(
                "LoadAnm[6] script #%d raw_id=%u global_id=%u byteOff=%u "
                "ptr=%p firstOpcode=%d firstTime=%d",
                index, curSpriteOffset[0], globalIdx, curSpriteOffset[1],
                (void *)scriptPtr,
                scriptPtr ? (int)((const u8 *)scriptPtr)[2] : -1,
                scriptPtr ? (int)utils::ReadUnaligned<i16>(scriptPtr) : -1);
        }
    }

    this->anmFilesSpriteIndexOffsets[anmIdx] = spriteIdxOffset;

    return ZUN_SUCCESS;
}

#pragma var_order(entry, spriteIdx, spriteIdxOffset, i, byteOffset, anmFilePtr, anmIdx, )
void AnmManager::ReleaseAnm(i32 anmIdx)
{
    if (this->anmFiles[anmIdx] != NULL)
    {
        i32 spriteIdxVal;
        i32 i;
        i32 spriteIdxOffset = this->anmFilesSpriteIndexOffsets[anmIdx];
        u32 *byteOffset = this->anmFiles[anmIdx]->spriteOffsets;
        for (i = 0; i < this->anmFiles[anmIdx]->numSprites; i++, byteOffset++)
        {
            spriteIdxVal = utils::ReadUnaligned<i32>((u8 *)this->anmFiles[anmIdx] + *byteOffset);
            memset(&this->sprites[spriteIdxVal + spriteIdxOffset], 0,
                   sizeof(this->sprites[spriteIdxVal + spriteIdxOffset]));
            this->sprites[spriteIdxVal + spriteIdxOffset].sourceFileIndex = -1;
        }

        for (i = 0; i < this->anmFiles[anmIdx]->numScripts; i++, byteOffset += 2)
        {
            this->scripts[*byteOffset + spriteIdxOffset] = NULL;
            this->spriteIndices[*byteOffset + spriteIdxOffset] = NULL;
        }
        this->anmFilesSpriteIndexOffsets[anmIdx] = NULL;
        AnmRawEntry *entry = this->anmFiles[anmIdx];
        this->ReleaseTexture(entry->textureIdx);
        AnmRawEntry *anmFilePtr = this->anmFiles[anmIdx];
        free(anmFilePtr);
        this->anmFiles[anmIdx] = 0;
        this->currentBlendMode = 0xff;
        this->currentColorOp = 0xff;
        this->currentVertexShader = 0xff;
        this->currentTexture = 0;
    }
}

void AnmManager::ReleaseTexture(i32 textureIdx)
{
    if (this->textures[textureIdx] != 0)
    {
        g_Renderer->DeleteTexture(this->textures[textureIdx]);
        this->textures[textureIdx] = 0;
    }

    void *imageDataArray = this->imageDataArray[textureIdx];
    free(imageDataArray);

    this->imageDataArray[textureIdx] = NULL;
}

void AnmManager::LoadSprite(u32 spriteIdx, AnmLoadedSprite *sprite)
{
    this->sprites[spriteIdx] = *sprite;
    this->sprites[spriteIdx].spriteId = this->maybeLoadedSpriteCount++;

    this->sprites[spriteIdx].uvStart.x =
        this->sprites[spriteIdx].startPixelInclusive.x / (this->sprites[spriteIdx].textureWidth);
    this->sprites[spriteIdx].uvEnd.x =
        this->sprites[spriteIdx].endPixelInclusive.x / (this->sprites[spriteIdx].textureWidth);
    this->sprites[spriteIdx].uvStart.y =
        this->sprites[spriteIdx].startPixelInclusive.y / (this->sprites[spriteIdx].textureHeight);
    this->sprites[spriteIdx].uvEnd.y =
        this->sprites[spriteIdx].endPixelInclusive.y / (this->sprites[spriteIdx].textureHeight);

    this->sprites[spriteIdx].widthPx =
        this->sprites[spriteIdx].endPixelInclusive.x - this->sprites[spriteIdx].startPixelInclusive.x;
    this->sprites[spriteIdx].heightPx =
        this->sprites[spriteIdx].endPixelInclusive.y - this->sprites[spriteIdx].startPixelInclusive.y;
}

ZunResult AnmManager::SetActiveSprite(AnmVm *vm, u32 sprite_index)
{
    if (this->sprites[sprite_index].sourceFileIndex < 0)
    {
        return ZUN_ERROR;
    }

    vm->activeSpriteIndex = (i16)sprite_index;
    vm->sprite = this->sprites + sprite_index;
    D3DXMatrixIdentity(&vm->matrix);
    vm->matrix.m[0][0] = vm->sprite->widthPx / vm->sprite->textureWidth;
    vm->matrix.m[1][1] = vm->sprite->heightPx / vm->sprite->textureHeight;

    return ZUN_SUCCESS;
}

void AnmManager::SetAndExecuteScript(AnmVm *vm, AnmRawInstr *beginingOfScript)
{
    ZunTimer *timer;

    // Diagnostic for item scripts (global index 533-540)
    if (vm->anmFileIndex >= 533 && vm->anmFileIndex <= 540)
    {
        DIAG_LOG_INFO(
            "ITEM_SETEXEC idx=%d script_ptr=%p isNull=%d",
            (int)vm->anmFileIndex, (void *)beginingOfScript, beginingOfScript == NULL);
    }

    vm->flags.flip = 0;
    vm->Initialize();
    vm->beginingOfScript = beginingOfScript;
    vm->currentInstruction = vm->beginingOfScript;

    timer = &(vm->currentTimeInScript);
    timer->current = 0;
    timer->subFrame = 0.0;
    timer->previous = -999;

    vm->flags.isVisible = 0;
    if (beginingOfScript)
    {
        this->ExecuteScript(vm);
    }

    // Post-execution diagnostic for item scripts
    if (vm->anmFileIndex >= 533 && vm->anmFileIndex <= 540)
    {
        DIAG_LOG_INFO(
            "ITEM_AFTEREXEC idx=%d isVis=%d flag1=%d activeSprite=%d color=0x%08X spritePtr=%p",
            (int)vm->anmFileIndex, (int)vm->flags.isVisible, (int)vm->flags.flag1,
            (int)vm->activeSpriteIndex, (unsigned)vm->color,
            (void *)vm->sprite);
    }
}

void AnmManager::SetRenderStateForVm(AnmVm *vm)
{
    if (this->currentBlendMode != vm->flags.blendMode)
    {
        this->currentBlendMode = vm->flags.blendMode;
        if (this->currentBlendMode == AnmVmBlendMode_InvSrcAlpha)
        {
            g_Renderer->SetBlendMode(BLEND_MODE_INV_SRC_ALPHA);
        }
        else
        {
            g_Renderer->SetBlendMode(BLEND_MODE_ADDITIVE);
        }
    }
    if ((((g_Supervisor.cfg.opts >> GCOS_USE_D3D_HW_TEXTURE_BLENDING) & 1) == 0) &&
        (((g_Supervisor.cfg.opts >> GCOS_NO_COLOR_COMP) & 1) == 0) && (this->currentColorOp != vm->flags.colorOp))
    {
        this->currentColorOp = vm->flags.colorOp;
        if (this->currentColorOp == AnmVmColorOp_Modulate)
        {
            g_Renderer->SetColorOp(0);
        }
        else
        {
            g_Renderer->SetColorOp(1);
        }
    }
    if (((g_Supervisor.cfg.opts >> GCOS_DONT_USE_VERTEX_BUF) & 1) == 0)
    {
        if (this->currentTextureFactor != vm->color)
        {
            this->currentTextureFactor = vm->color;
            g_Renderer->SetTextureFactor(this->currentTextureFactor);
        }
    }
    else
    {
        g_PrimitivesToDrawNoVertexBuf[0].diffuse = vm->color;
        g_PrimitivesToDrawNoVertexBuf[1].diffuse = vm->color;
        g_PrimitivesToDrawNoVertexBuf[2].diffuse = vm->color;
        g_PrimitivesToDrawNoVertexBuf[3].diffuse = vm->color;
        g_PrimitivesToDrawUnknown[0].diffuse = vm->color;
        g_PrimitivesToDrawUnknown[1].diffuse = vm->color;
        g_PrimitivesToDrawUnknown[2].diffuse = vm->color;
        g_PrimitivesToDrawUnknown[3].diffuse = vm->color;
    }
    if ((((g_Supervisor.cfg.opts >> GCOS_TURN_OFF_DEPTH_TEST) & 1) == 0) &&
        (this->currentZWriteDisable != vm->flags.zWriteDisable))
    {
        this->currentZWriteDisable = vm->flags.zWriteDisable;
        g_Renderer->SetZWriteDisable(this->currentZWriteDisable);
    }
    return;
}

static f32 g_ZeroPointFive = 0.5;

ZunResult AnmManager::DrawInner(AnmVm *vm, i32 param_3)
{
    if (param_3 != 0)
    {
        f32 x0 = roundf(g_PrimitivesToDrawVertexBuf[0].position.x);
        f32 x1 = roundf(g_PrimitivesToDrawVertexBuf[1].position.x);
        f32 y0 = roundf(g_PrimitivesToDrawVertexBuf[0].position.y);
        f32 y2 = roundf(g_PrimitivesToDrawVertexBuf[2].position.y);
        g_PrimitivesToDrawVertexBuf[2].position.y = y2;
        g_PrimitivesToDrawVertexBuf[3].position.y = y2;
        g_PrimitivesToDrawVertexBuf[0].position.y = y0;
        g_PrimitivesToDrawVertexBuf[1].position.y = y0;
        g_PrimitivesToDrawVertexBuf[1].position.x = x1;
        g_PrimitivesToDrawVertexBuf[3].position.x = x1;
        g_PrimitivesToDrawVertexBuf[0].position.x = x0;
        g_PrimitivesToDrawVertexBuf[2].position.x = x0;
    }
    g_PrimitivesToDrawVertexBuf[0].position.z = g_PrimitivesToDrawVertexBuf[1].position.z =
        g_PrimitivesToDrawVertexBuf[2].position.z = g_PrimitivesToDrawVertexBuf[3].position.z = vm->pos.z;
    // UV scroll may change each frame even when the sprite pointer stays the same.
    g_PrimitivesToDrawVertexBuf[0].textureUV.x = g_PrimitivesToDrawVertexBuf[2].textureUV.x =
        vm->sprite->uvStart.x + vm->uvScrollPos.x;
    g_PrimitivesToDrawVertexBuf[1].textureUV.x = g_PrimitivesToDrawVertexBuf[3].textureUV.x =
        vm->sprite->uvEnd.x + vm->uvScrollPos.x;
    g_PrimitivesToDrawVertexBuf[0].textureUV.y = g_PrimitivesToDrawVertexBuf[1].textureUV.y =
        vm->sprite->uvStart.y + vm->uvScrollPos.y;
    g_PrimitivesToDrawVertexBuf[2].textureUV.y = g_PrimitivesToDrawVertexBuf[3].textureUV.y =
        vm->sprite->uvEnd.y + vm->uvScrollPos.y;

    if (this->currentSprite != vm->sprite)
    {
        this->currentSprite = vm->sprite;
    }

    if (this->currentTexture != this->textures[vm->sprite->sourceFileIndex])
    {
        this->currentTexture = this->textures[vm->sprite->sourceFileIndex];
        g_Renderer->SetTexture(this->currentTexture);
    }
    if (this->currentVertexShader != 2)
    {
        this->currentVertexShader = 2;
    }
    this->SetRenderStateForVm(vm);

    // Diagnostic: log state for item sprites to identify
    // the "background bleeding into point items" bug on Android Stage 1.
    // 512=power, 513=point, 519=power_above, 520=point_above
    if (vm->activeSpriteIndex >= 512 && vm->activeSpriteIndex <= 520)
    {
        static i32 diagFrameCounter = 0;
        static i32 lastLoggedSprite = -1;
        diagFrameCounter++;
        // Log each sprite type once every ~2 seconds
        if (diagFrameCounter % 120 == 1 || lastLoggedSprite != vm->activeSpriteIndex)
        {
            lastLoggedSprite = vm->activeSpriteIndex;
            GLint glBoundTex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &glBoundTex);
            DIAG_LOG(
                "ITEM sprite=%d srcFileIdx=%d tex[srcIdx]=%u anmCurTex=%u rendCurTex=%u glBound=%d "
                "uv=[%.4f,%.4f]-[%.4f,%.4f] color=0x%08X blend=%d colorOp=%d flags=0x%04X",
                (int)vm->activeSpriteIndex,
                vm->sprite->sourceFileIndex,
                this->textures[vm->sprite->sourceFileIndex],
                this->currentTexture,
                g_Renderer->currentTexture,
                glBoundTex,
                vm->sprite->uvStart.x, vm->sprite->uvStart.y,
                vm->sprite->uvEnd.x, vm->sprite->uvEnd.y,
                (unsigned)vm->color,
                (int)vm->flags.blendMode, (int)vm->flags.colorOp,
                (unsigned)vm->flags.flags);
        }
    }

    if (((g_Supervisor.cfg.opts >> GCOS_DONT_USE_VERTEX_BUF) & 1) == 0)
    {
        g_Renderer->DrawTriangleStripTex(g_PrimitivesToDrawVertexBuf, 4);
    }
    else
    {
        g_PrimitivesToDrawNoVertexBuf[0].position.x = g_PrimitivesToDrawVertexBuf[0].position.x;
        g_PrimitivesToDrawNoVertexBuf[0].position.y = g_PrimitivesToDrawVertexBuf[0].position.y;
        g_PrimitivesToDrawNoVertexBuf[0].position.z = g_PrimitivesToDrawVertexBuf[0].position.z;
        g_PrimitivesToDrawNoVertexBuf[1].position.x = g_PrimitivesToDrawVertexBuf[1].position.x;
        g_PrimitivesToDrawNoVertexBuf[1].position.y = g_PrimitivesToDrawVertexBuf[1].position.y;
        g_PrimitivesToDrawNoVertexBuf[1].position.z = g_PrimitivesToDrawVertexBuf[1].position.z;
        g_PrimitivesToDrawNoVertexBuf[2].position.x = g_PrimitivesToDrawVertexBuf[2].position.x;
        g_PrimitivesToDrawNoVertexBuf[2].position.y = g_PrimitivesToDrawVertexBuf[2].position.y;
        g_PrimitivesToDrawNoVertexBuf[2].position.z = g_PrimitivesToDrawVertexBuf[2].position.z;
        g_PrimitivesToDrawNoVertexBuf[3].position.x = g_PrimitivesToDrawVertexBuf[3].position.x;
        g_PrimitivesToDrawNoVertexBuf[3].position.y = g_PrimitivesToDrawVertexBuf[3].position.y;
        g_PrimitivesToDrawNoVertexBuf[3].position.z = g_PrimitivesToDrawVertexBuf[3].position.z;
        g_PrimitivesToDrawNoVertexBuf[0].textureUV.x = g_PrimitivesToDrawNoVertexBuf[2].textureUV.x =
            vm->sprite->uvStart.x + vm->uvScrollPos.x;
        g_PrimitivesToDrawNoVertexBuf[1].textureUV.x = g_PrimitivesToDrawNoVertexBuf[3].textureUV.x =
            vm->sprite->uvEnd.x + vm->uvScrollPos.x;
        g_PrimitivesToDrawNoVertexBuf[0].textureUV.y = g_PrimitivesToDrawNoVertexBuf[1].textureUV.y =
            vm->sprite->uvStart.y + vm->uvScrollPos.y;
        g_PrimitivesToDrawNoVertexBuf[2].textureUV.y = g_PrimitivesToDrawNoVertexBuf[3].textureUV.y =
            vm->sprite->uvEnd.y + vm->uvScrollPos.y;
        g_Renderer->DrawTriangleStripTextured(g_PrimitivesToDrawNoVertexBuf, 4);
    }
    return ZUN_SUCCESS;
}

ZunResult AnmManager::DrawNoRotation(AnmVm *vm)
{
    float fVar2;
    float fVar3;

    if (vm->flags.isVisible == 0)
    {
        return ZUN_ERROR;
    }
    if (vm->flags.flag1 == 0)
    {
        return ZUN_ERROR;
    }
    if (vm->color == 0)
    {
        return ZUN_ERROR;
    }
    fVar2 = (vm->sprite->widthPx * vm->scaleX) / 2.0f;
    fVar3 = (vm->sprite->heightPx * vm->scaleY) / 2.0f;
    if ((vm->flags.anchor & AnmVmAnchor_Left) == 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.x = g_PrimitivesToDrawVertexBuf[2].position.x = vm->pos.x - fVar2;
        g_PrimitivesToDrawVertexBuf[1].position.x = g_PrimitivesToDrawVertexBuf[3].position.x = fVar2 + vm->pos.x;
    }
    else
    {
        g_PrimitivesToDrawVertexBuf[0].position.x = g_PrimitivesToDrawVertexBuf[2].position.x = vm->pos.x;
        g_PrimitivesToDrawVertexBuf[1].position.x = g_PrimitivesToDrawVertexBuf[3].position.x =
            fVar2 + vm->pos.x + fVar2;
    }
    if ((vm->flags.anchor & AnmVmAnchor_Top) == 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.y = g_PrimitivesToDrawVertexBuf[1].position.y = vm->pos.y - fVar3;
        g_PrimitivesToDrawVertexBuf[2].position.y = g_PrimitivesToDrawVertexBuf[3].position.y = fVar3 + vm->pos.y;
    }
    else
    {
        g_PrimitivesToDrawVertexBuf[0].position.y = g_PrimitivesToDrawVertexBuf[1].position.y = vm->pos.y;
        g_PrimitivesToDrawVertexBuf[2].position.y = g_PrimitivesToDrawVertexBuf[3].position.y =
            fVar3 + vm->pos.y + fVar3;
    }
    return this->DrawInner(vm, 1);
}

void AnmManager::TranslateRotation(VertexTex1Xyzrwh *param_1, f32 x, f32 y, f32 sine, f32 cosine, f32 xOffset,
                                   f32 yOffset)
{
    param_1->position.x = x * cosine + y * sine + xOffset;
    param_1->position.y = -x * sine + y * cosine + yOffset;
    return;
}

#pragma var_order(spriteXCenter, spriteYCenter, yOffset, xOffset, zSine, z, zCosine)
ZunResult AnmManager::Draw(AnmVm *vm)
{
    f32 zSine;
    f32 zCosine;
    f32 spriteXCenter;
    f32 spriteYCenter;
    f32 xOffset;
    f32 yOffset;
    f32 z;

    if (vm->rotation.z == 0.0f)
    {
        return this->DrawNoRotation(vm);
    }
    if (vm->flags.isVisible == 0)
    {
        return ZUN_ERROR;
    }
    if (vm->flags.flag1 == 0)
    {
        return ZUN_ERROR;
    }
    if (vm->color == 0)
    {
        return ZUN_ERROR;
    }
    z = vm->rotation.z;
    sincos(z, zSine, zCosine);
    xOffset = rintf(vm->pos.x);
    yOffset = rintf(vm->pos.y);
    spriteXCenter = rintf((vm->sprite->widthPx * vm->scaleX) / 2.0f);
    spriteYCenter = rintf((vm->sprite->heightPx * vm->scaleY) / 2.0f);
    this->TranslateRotation(&g_PrimitivesToDrawVertexBuf[0], -spriteXCenter, -spriteYCenter, zSine,
                            zCosine, xOffset, yOffset);
    this->TranslateRotation(&g_PrimitivesToDrawVertexBuf[1], spriteXCenter, -spriteYCenter, zSine,
                            zCosine, xOffset, yOffset);
    this->TranslateRotation(&g_PrimitivesToDrawVertexBuf[2], -spriteXCenter, spriteYCenter, zSine,
                            zCosine, xOffset, yOffset);
    this->TranslateRotation(&g_PrimitivesToDrawVertexBuf[3], spriteXCenter, spriteYCenter, zSine, zCosine,
                            xOffset, yOffset);
    g_PrimitivesToDrawVertexBuf[0].position.z = g_PrimitivesToDrawVertexBuf[1].position.z =
        g_PrimitivesToDrawVertexBuf[2].position.z = g_PrimitivesToDrawVertexBuf[3].position.z = vm->pos.z;
    if ((vm->flags.anchor & AnmVmAnchor_Left) != 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.x += spriteXCenter;
        g_PrimitivesToDrawVertexBuf[1].position.x += spriteXCenter;
        g_PrimitivesToDrawVertexBuf[2].position.x += spriteXCenter;
        g_PrimitivesToDrawVertexBuf[3].position.x += spriteXCenter;
    }
    if ((vm->flags.anchor & AnmVmAnchor_Top) != 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.y += spriteYCenter;
        g_PrimitivesToDrawVertexBuf[1].position.y += spriteYCenter;
        g_PrimitivesToDrawVertexBuf[2].position.y += spriteYCenter;
        g_PrimitivesToDrawVertexBuf[3].position.y += spriteYCenter;
    }
    return this->DrawInner(vm, 0);
}

ZunResult AnmManager::DrawFacingCamera(AnmVm *vm)
{
    f32 centerX;
    f32 centerY;

    if (!vm->flags.isVisible)
    {
        return ZUN_ERROR;
    }
    if (!vm->flags.flag1)
    {
        return ZUN_ERROR;
    }
    if (vm->color == 0)
    {
        return ZUN_ERROR;
    }

    centerX = vm->sprite->widthPx * vm->scaleX / 2.0f;
    centerY = vm->sprite->heightPx * vm->scaleY / 2.0f;
    if ((vm->flags.anchor & AnmVmAnchor_Left) == 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.x = g_PrimitivesToDrawVertexBuf[2].position.x = vm->pos.x - centerX;
        g_PrimitivesToDrawVertexBuf[1].position.x = g_PrimitivesToDrawVertexBuf[3].position.x = vm->pos.x + centerX;
    }
    else
    {
        g_PrimitivesToDrawVertexBuf[0].position.x = g_PrimitivesToDrawVertexBuf[2].position.x = vm->pos.x;
        g_PrimitivesToDrawVertexBuf[1].position.x = g_PrimitivesToDrawVertexBuf[3].position.x =
            vm->pos.x + centerX + centerX;
    }
    if ((vm->flags.anchor & AnmVmAnchor_Top) == 0)
    {
        g_PrimitivesToDrawVertexBuf[0].position.y = g_PrimitivesToDrawVertexBuf[1].position.y = vm->pos.y - centerY;
        g_PrimitivesToDrawVertexBuf[2].position.y = g_PrimitivesToDrawVertexBuf[3].position.y = vm->pos.y + centerY;
    }
    else
    {
        g_PrimitivesToDrawVertexBuf[0].position.y = g_PrimitivesToDrawVertexBuf[1].position.y = vm->pos.y;
        g_PrimitivesToDrawVertexBuf[2].position.y = g_PrimitivesToDrawVertexBuf[3].position.y =
            vm->pos.y + centerY + centerY;
    }
    return this->DrawInner(vm, 0);
}

#pragma var_order(textureMatrix, rotationMatrix, worldTransformMatrix, scaledXCenter, scaledYCenter)
ZunResult AnmManager::Draw3(AnmVm *vm)
{
    D3DXMATRIX worldTransformMatrix;
    D3DXMATRIX rotationMatrix;
    D3DXMATRIX textureMatrix;
    f32 scaledXCenter;
    f32 scaledYCenter;

    if (!vm->flags.isVisible)
    {
        return ZUN_ERROR;
    }
    if (!vm->flags.flag1)
    {
        return ZUN_ERROR;
    }
    if (vm->color == 0)
    {
        return ZUN_ERROR;
    }

    worldTransformMatrix = vm->matrix;
    worldTransformMatrix.m[0][0] *= vm->scaleX;
    worldTransformMatrix.m[1][1] *= -vm->scaleY;

    if (vm->rotation.x != 0.0)
    {
        D3DXMatrixRotationX(&rotationMatrix, vm->rotation.x);
        D3DXMatrixMultiply(&worldTransformMatrix, &worldTransformMatrix, &rotationMatrix);
    }

    if (vm->rotation.y != 0.0)
    {
        D3DXMatrixRotationY(&rotationMatrix, vm->rotation.y);
        D3DXMatrixMultiply(&worldTransformMatrix, &worldTransformMatrix, &rotationMatrix);
    }

    if (vm->rotation.z != 0.0)
    {
        D3DXMatrixRotationZ(&rotationMatrix, vm->rotation.z);
        D3DXMatrixMultiply(&worldTransformMatrix, &worldTransformMatrix, &rotationMatrix);
    }

    if ((vm->flags.anchor & AnmVmAnchor_Left) == 0)
    {
        worldTransformMatrix.m[3][0] = vm->pos.x;
    }
    else
    {
        scaledXCenter = vm->sprite->widthPx * vm->scaleX / 2.0f;
        worldTransformMatrix.m[3][0] = fabsf(scaledXCenter) + vm->pos.x;
    }

    if ((vm->flags.anchor & AnmVmAnchor_Top) == 0)
    {
        worldTransformMatrix.m[3][1] = -vm->pos.y;
    }
    else
    {
        scaledYCenter = vm->sprite->heightPx * vm->scaleY / 2.0f;
        worldTransformMatrix.m[3][1] = -vm->pos.y - fabsf(scaledYCenter);
    }

    worldTransformMatrix.m[3][2] = vm->pos.z;

    g_Renderer->SetWorldTransform(&worldTransformMatrix);

    // Explicit UV assignment is more robust than relying on fixed-pipeline texture matrix semantics.
    this->vertexBufferContents[0].textureUV.x = this->vertexBufferContents[2].textureUV.x =
        vm->sprite->uvStart.x + vm->uvScrollPos.x;
    this->vertexBufferContents[1].textureUV.x = this->vertexBufferContents[3].textureUV.x =
        vm->sprite->uvEnd.x + vm->uvScrollPos.x;
    this->vertexBufferContents[0].textureUV.y = this->vertexBufferContents[1].textureUV.y =
        vm->sprite->uvStart.y + vm->uvScrollPos.y;
    this->vertexBufferContents[2].textureUV.y = this->vertexBufferContents[3].textureUV.y =
        vm->sprite->uvEnd.y + vm->uvScrollPos.y;

    D3DXMATRIX textureIdentity;
    D3DXMatrixIdentity(&textureIdentity);
    g_Renderer->SetTextureTransform(&textureIdentity);

    if (this->currentSprite != vm->sprite)
    {
        this->currentSprite = vm->sprite;
    }
    if (this->currentTexture != this->textures[vm->sprite->sourceFileIndex])
    {
        this->currentTexture = this->textures[vm->sprite->sourceFileIndex];
        g_Renderer->SetTexture(this->currentTexture);
    }

    if (this->currentVertexShader != 3)
    {
        this->currentVertexShader = 3;
    }

    this->SetRenderStateForVm(vm);

    g_Renderer->DrawVertexBuffer3D(this->vertexBufferContents, 4);
    return ZUN_SUCCESS;
}

#pragma var_order(textureMatrix, unusedMatrix, worldTransformMatrix)
ZunResult AnmManager::Draw2(AnmVm *vm)
{
    D3DXMATRIX worldTransformMatrix;
    D3DXMATRIX unusedMatrix;
    D3DXMATRIX textureMatrix;

    if (!vm->flags.isVisible)
    {
        return ZUN_ERROR;
    }
    if (!vm->flags.flag1)
    {
        return ZUN_ERROR;
    }

    if (vm->rotation.x != 0 || vm->rotation.y != 0 || vm->rotation.z != 0)
    {
        return this->Draw3(vm);
    }

    if (vm->color == 0)
    {
        return ZUN_ERROR;
    }

    worldTransformMatrix = vm->matrix;
    worldTransformMatrix.m[3][0] = rintf(vm->pos.x);
    worldTransformMatrix.m[3][1] = -rintf(vm->pos.y);
    if ((vm->flags.anchor & AnmVmAnchor_Left) != 0)
    {
        worldTransformMatrix.m[3][0] += (vm->sprite->widthPx * vm->scaleX) / 2.0f;
    }
    if ((vm->flags.anchor & AnmVmAnchor_Top) != 0)
    {
        worldTransformMatrix.m[3][1] -= (vm->sprite->heightPx * vm->scaleY) / 2.0f;
    }
    worldTransformMatrix.m[3][2] = vm->pos.z;
    worldTransformMatrix.m[0][0] *= vm->scaleX;
    worldTransformMatrix.m[1][1] *= -vm->scaleY;
    g_Renderer->SetWorldTransform(&worldTransformMatrix);

    this->vertexBufferContents[0].textureUV.x = this->vertexBufferContents[2].textureUV.x =
        vm->sprite->uvStart.x + vm->uvScrollPos.x;
    this->vertexBufferContents[1].textureUV.x = this->vertexBufferContents[3].textureUV.x =
        vm->sprite->uvEnd.x + vm->uvScrollPos.x;
    this->vertexBufferContents[0].textureUV.y = this->vertexBufferContents[1].textureUV.y =
        vm->sprite->uvStart.y + vm->uvScrollPos.y;
    this->vertexBufferContents[2].textureUV.y = this->vertexBufferContents[3].textureUV.y =
        vm->sprite->uvEnd.y + vm->uvScrollPos.y;

    D3DXMATRIX textureIdentity;
    D3DXMatrixIdentity(&textureIdentity);
    g_Renderer->SetTextureTransform(&textureIdentity);

    if (this->currentSprite != vm->sprite)
    {
        this->currentSprite = vm->sprite;
    }
    if (this->currentTexture != this->textures[vm->sprite->sourceFileIndex])
    {
        this->currentTexture = this->textures[vm->sprite->sourceFileIndex];
        g_Renderer->SetTexture(this->currentTexture);
    }
    if (this->currentVertexShader != 3)
    {
        this->currentVertexShader = 3;
    }
    this->SetRenderStateForVm(vm);
    g_Renderer->DrawVertexBuffer3D(this->vertexBufferContents, 4);
    return ZUN_SUCCESS;
}

#pragma var_order(curInstr, local_c, local_10, local_14, local_18, local_1c, local_20, nextInstr, local_28, local_2c,  \
                  local_30, local_34, local_38, local_3c)
i32 AnmManager::ExecuteScript(AnmVm *vm)
{
    AnmRawInstr *curInstr;
    u32 *local_c;
    f32 *local_10;
    f32 *local_14;
    f32 *local_18;
    f32 *local_1c;
    u32 *local_20;
    AnmRawInstr *nextInstr;
    ZunColor local_28;
    ZunColor local_2c;
    f32 local_30;
    i32 local_34;
    i32 local_38;
    f32 local_3c;
    const u8 *curInstrArgs;

    if (vm->currentInstruction == NULL)
    {
        return 1;
    }

    auto instructionTime = [](AnmRawInstr *instr) -> i16 { return utils::ReadUnaligned<i16>(instr); };
    auto instructionOpcode = [](AnmRawInstr *instr) -> u8 { return reinterpret_cast<const u8 *>(instr)[2]; };
    auto instructionArgsCount = [](AnmRawInstr *instr) -> u8 { return reinterpret_cast<const u8 *>(instr)[3]; };
    auto instructionArgs = [](AnmRawInstr *instr) -> const u8 * { return reinterpret_cast<const u8 *>(instr) + 4; };
    auto nextInstruction = [&](AnmRawInstr *instr) -> AnmRawInstr * {
        return reinterpret_cast<AnmRawInstr *>(
            const_cast<u8 *>(instructionArgs(instr) + instructionArgsCount(instr)));
    };
    auto readArgU32 = [](const u8 *args, u32 index) -> u32 {
        return utils::ReadUnaligned<u32>(args + index * sizeof(u32));
    };
    auto readArgF32 = [](const u8 *args, u32 index) -> f32 {
        return utils::ReadUnaligned<f32>(args + index * sizeof(u32));
    };

    if (vm->pendingInterrupt != 0)
    {
        goto yolo;
    }

    while (curInstr = vm->currentInstruction, instructionTime(curInstr) <= vm->currentTimeInScript.AsFrames())
    {
        curInstrArgs = instructionArgs(curInstr);

        switch (instructionOpcode(curInstr))
        {
        case AnmOpcode_Exit:
            vm->flags.isVisible = 0;
        case AnmOpcode_ExitHide:
            vm->currentInstruction = NULL;
            return 1;
        case AnmOpcode_SetActiveSprite:
            vm->flags.isVisible = 1;
            this->SetActiveSprite(vm, readArgU32(curInstrArgs, 0) + this->spriteIndices[vm->anmFileIndex]);
            vm->timeOfLastSpriteSet = vm->currentTimeInScript.AsFrames();
            break;
        case AnmOpcode_SetRandomSprite:
            vm->flags.isVisible = 1;
            this->SetActiveSprite(vm, readArgU32(curInstrArgs, 0) + g_Rng.GetRandomU16InRange(readArgU32(curInstrArgs, 1)) +
                                          this->spriteIndices[vm->anmFileIndex]);
            vm->timeOfLastSpriteSet = vm->currentTimeInScript.AsFrames();
            break;
        case AnmOpcode_SetScale:
            vm->scaleX = readArgF32(curInstrArgs, 0);
            vm->scaleY = readArgF32(curInstrArgs, 1);
            break;
        case AnmOpcode_SetAlpha:
            COLOR_SET_COMPONENT(vm->color, COLOR_ALPHA_BYTE_IDX, readArgU32(curInstrArgs, 0) & 0xff);
            break;
        case AnmOpcode_SetColor:
            vm->color = COLOR_COMBINE_ALPHA(readArgU32(curInstrArgs, 0), vm->color);
            break;
        case AnmOpcode_Jump:
            vm->currentInstruction = reinterpret_cast<AnmRawInstr *>(
                const_cast<u8 *>(reinterpret_cast<const u8 *>(vm->beginingOfScript) + readArgU32(curInstrArgs, 0)));
            vm->currentTimeInScript.current = instructionTime(vm->currentInstruction);
            continue;
        case AnmOpcode_FlipX:
            vm->flags.flip ^= 1;
            vm->scaleX *= -1.f;
            break;
        case AnmOpcode_UsePosOffset:
            vm->flags.usePosOffset = readArgU32(curInstrArgs, 0);
            break;
        case AnmOpcode_FlipY:
            vm->flags.flip ^= 2;
            vm->scaleY *= -1.f;
            break;
        case AnmOpcode_SetRotation:
            vm->rotation.x = readArgF32(curInstrArgs, 0);
            vm->rotation.y = readArgF32(curInstrArgs, 1);
            vm->rotation.z = readArgF32(curInstrArgs, 2);
            break;
        case AnmOpcode_SetAngleVel:
            vm->angleVel.x = readArgF32(curInstrArgs, 0);
            vm->angleVel.y = readArgF32(curInstrArgs, 1);
            vm->angleVel.z = readArgF32(curInstrArgs, 2);
            break;
        case AnmOpcode_SetScaleSpeed:
            vm->scaleInterpFinalX = readArgF32(curInstrArgs, 0);
            vm->scaleInterpFinalY = readArgF32(curInstrArgs, 1);
            vm->scaleInterpEndTime = 0;
            break;
        case AnmOpcode_ScaleTime:
            vm->scaleInterpFinalX = readArgF32(curInstrArgs, 0);
            vm->scaleInterpFinalY = readArgF32(curInstrArgs, 1);
            vm->scaleInterpEndTime = utils::ReadUnaligned<u16>(curInstrArgs + sizeof(f32) * 2);
            vm->scaleInterpTime.InitializeForPopup();
            vm->scaleInterpInitialX = vm->scaleX;
            vm->scaleInterpInitialY = vm->scaleY;
            break;
        case AnmOpcode_Fade:
            vm->alphaInterpInitial = vm->color;
            vm->alphaInterpFinal = COLOR_SET_ALPHA2(vm->color, readArgU32(curInstrArgs, 0));
            vm->alphaInterpEndTime = readArgU32(curInstrArgs, 1);
            vm->alphaInterpTime.InitializeForPopup();
            break;
        case AnmOpcode_SetBlendAdditive:
            vm->flags.blendMode = AnmVmBlendMode_One;
            break;
        case AnmOpcode_SetBlendDefault:
            vm->flags.blendMode = AnmVmBlendMode_InvSrcAlpha;
            break;
        case AnmOpcode_SetPosition:
            if (vm->flags.usePosOffset == 0)
            {
                vm->pos = D3DXVECTOR3(readArgF32(curInstrArgs, 0), readArgF32(curInstrArgs, 1),
                                      readArgF32(curInstrArgs, 2));
            }
            else
            {
                vm->posOffset = D3DXVECTOR3(readArgF32(curInstrArgs, 0), readArgF32(curInstrArgs, 1),
                                            readArgF32(curInstrArgs, 2));
            }
            break;
        case AnmOpcode_PosTimeAccel:
            vm->flags.posTime = 2;
            goto PosTimeDoStuff;
        case AnmOpcode_PosTimeDecel:
            vm->flags.posTime = 1;
            goto PosTimeDoStuff;
        case AnmOpcode_PosTimeLinear:
            vm->flags.posTime = 0;
        PosTimeDoStuff:
            if (vm->flags.usePosOffset == 0)
            {
                memcpy(vm->posInterpInitial, vm->pos, sizeof(D3DXVECTOR3));
            }
            else
            {
                memcpy(vm->posInterpInitial, vm->posOffset, sizeof(D3DXVECTOR3));
            }
            vm->posInterpFinal = D3DXVECTOR3(readArgF32(curInstrArgs, 0), readArgF32(curInstrArgs, 1),
                                             readArgF32(curInstrArgs, 2));
            vm->posInterpEndTime = readArgU32(curInstrArgs, 3);
            vm->posInterpTime.InitializeForPopup();
            break;
        case AnmOpcode_StopHide:
            vm->flags.isVisible = 0;
        case AnmOpcode_Stop:
            if (vm->pendingInterrupt == 0)
            {
                vm->flags.isStopped = 1;
                vm->currentTimeInScript.Decrement(1);
                goto stop;
            }
        yolo:
            nextInstr = NULL;
            curInstr = vm->beginingOfScript;
            while ((instructionOpcode(curInstr) != AnmOpcode_InterruptLabel ||
                    vm->pendingInterrupt != readArgU32(instructionArgs(curInstr), 0)) &&
                   instructionOpcode(curInstr) != AnmOpcode_Exit && instructionOpcode(curInstr) != AnmOpcode_ExitHide)
            {
                if (instructionOpcode(curInstr) == AnmOpcode_InterruptLabel &&
                    readArgU32(instructionArgs(curInstr), 0) == 0xffffffff)
                {
                    nextInstr = curInstr;
                }
                curInstr = nextInstruction(curInstr);
            }

            vm->pendingInterrupt = 0;
            vm->flags.isStopped = 0;
            if (instructionOpcode(curInstr) != AnmOpcode_InterruptLabel)
            {
                if (nextInstr == NULL)
                {
                    vm->currentTimeInScript.Decrement(1);
                    goto stop;
                }
                curInstr = nextInstr;
            }

            curInstr = nextInstruction(curInstr);
            vm->currentInstruction = curInstr;
            vm->currentTimeInScript.SetCurrent(instructionTime(vm->currentInstruction));
            vm->flags.isVisible = 1;
            continue;
        case AnmOpcode_SetVisibility:
            vm->flags.isVisible = readArgU32(curInstrArgs, 0);
            break;
        case AnmOpcode_AnchorTopLeft:
            vm->flags.anchor = AnmVmAnchor_TopLeft;
            break;
        case AnmOpcode_SetAutoRotate:
            vm->autoRotate = readArgU32(curInstrArgs, 0);
            break;
        case AnmOpcode_UVScrollX:
            vm->uvScrollPos.x += readArgF32(curInstrArgs, 0);
            if (vm->uvScrollPos.x >= 1.0f)
            {
                vm->uvScrollPos.x -= 1.0f;
            }
            else if (vm->uvScrollPos.x < 0.0f)
            {
                vm->uvScrollPos.x += 1.0f;
            }
            break;
        case AnmOpcode_UVScrollY:
            vm->uvScrollPos.y += readArgF32(curInstrArgs, 0);
            if (vm->uvScrollPos.y >= 1.0f)
            {
                vm->uvScrollPos.y -= 1.0f;
            }
            else if (vm->uvScrollPos.y < 0.0f)
            {
                vm->uvScrollPos.y += 1.0f;
            }
            break;
        case AnmOpcode_SetZWriteDisable:
            vm->flags.zWriteDisable = readArgU32(curInstrArgs, 0);
            break;
        case AnmOpcode_Nop:
        case AnmOpcode_InterruptLabel:
        default:
            break;
        }
        vm->currentInstruction = nextInstruction(curInstr);
    }

stop:
    if (vm->angleVel.x != 0.0f)
    {
        vm->rotation.x =
            utils::AddNormalizeAngle(vm->rotation.x, g_Supervisor.effectiveFramerateMultiplier * vm->angleVel.x);
    }
    if (vm->angleVel.y != 0.0f)
    {
        vm->rotation.y =
            utils::AddNormalizeAngle(vm->rotation.y, g_Supervisor.effectiveFramerateMultiplier * vm->angleVel.y);
    }
    if (vm->angleVel.z != 0.0f)
    {
        vm->rotation.z =
            utils::AddNormalizeAngle(vm->rotation.z, g_Supervisor.effectiveFramerateMultiplier * vm->angleVel.z);
    }
    if (vm->scaleInterpEndTime > 0)
    {
        vm->scaleInterpTime.Tick();
        if (vm->scaleInterpTime.AsFrames() >= vm->scaleInterpEndTime)
        {
            vm->scaleY = vm->scaleInterpFinalY;
            vm->scaleX = vm->scaleInterpFinalX;
            vm->scaleInterpEndTime = 0;
            vm->scaleInterpFinalY = 0.0;
            vm->scaleInterpFinalX = 0.0;
        }
        else
        {
            vm->scaleX = (vm->scaleInterpFinalX - vm->scaleInterpInitialX) * vm->scaleInterpTime.AsFramesFloat() /
                             vm->scaleInterpEndTime +
                         vm->scaleInterpInitialX;
            vm->scaleY = (vm->scaleInterpFinalY - vm->scaleInterpInitialY) * vm->scaleInterpTime.AsFramesFloat() /
                             vm->scaleInterpEndTime +
                         vm->scaleInterpInitialY;
        }
        if ((vm->flags.flip & 1) != 0)
        {
            vm->scaleX = vm->scaleX * -1.f;
        }
        if ((vm->flags.flip & 2) != 0)
        {
            vm->scaleY = vm->scaleY * -1.f;
        }
    }
    else
    {
        vm->scaleY = g_Supervisor.effectiveFramerateMultiplier * vm->scaleInterpFinalY + vm->scaleY;
        vm->scaleX = g_Supervisor.effectiveFramerateMultiplier * vm->scaleInterpFinalX + vm->scaleX;
    }
    if (0 < vm->alphaInterpEndTime)
    {
        vm->alphaInterpTime.Tick();
        local_2c = vm->alphaInterpInitial;
        local_28 = vm->alphaInterpFinal;
        local_30 = vm->alphaInterpTime.AsFramesFloat() / (f32)vm->alphaInterpEndTime;
        if (local_30 >= 1.0f)
        {
            local_30 = 1.0;
        }
        for (local_38 = 0; local_38 < 4; local_38++)
        {
            local_34 = ((f32)COLOR_GET_COMPONENT(local_28, local_38) - (f32)COLOR_GET_COMPONENT(local_2c, local_38)) *
                           local_30 +
                       COLOR_GET_COMPONENT(local_2c, local_38);
            if (local_34 < 0)
            {
                local_34 = 0;
            }
            COLOR_SET_COMPONENT(local_2c, local_38, local_34 >= 256 ? 255 : local_34);
        }
        vm->color = local_2c;
        if (vm->alphaInterpTime.AsFrames() >= vm->alphaInterpEndTime)
        {
            vm->alphaInterpEndTime = 0;
        }
    }
    if (vm->posInterpEndTime != 0)
    {
        local_3c = vm->posInterpTime.AsFramesFloat() / (f32)vm->posInterpEndTime;
        if (local_3c >= 1.0f)
        {
            local_3c = 1.0;
        }
        switch (vm->flags.posTime)
        {
        case 1:
            local_3c = 1.0f - local_3c;
            local_3c *= local_3c;
            local_3c = 1.0f - local_3c;
            break;
        case 2:
            local_3c = 1.0f - local_3c;
            local_3c = local_3c * local_3c * local_3c * local_3c;
            local_3c = 1.0f - local_3c;
            break;
        }
        if (vm->flags.usePosOffset == 0)
        {
            vm->pos.x = local_3c * vm->posInterpFinal.x + (1.0f - local_3c) * vm->posInterpInitial.x;
            vm->pos.y = local_3c * vm->posInterpFinal.y + (1.0f - local_3c) * vm->posInterpInitial.y;
            vm->pos.z = local_3c * vm->posInterpFinal.z + (1.0f - local_3c) * vm->posInterpInitial.z;
        }
        else
        {
            vm->posOffset.x = local_3c * vm->posInterpFinal.x + (1.0f - local_3c) * vm->posInterpInitial.x;
            vm->posOffset.y = local_3c * vm->posInterpFinal.y + (1.0f - local_3c) * vm->posInterpInitial.y;
            vm->posOffset.z = local_3c * vm->posInterpFinal.z + (1.0f - local_3c) * vm->posInterpInitial.z;
        }

        if (vm->posInterpTime.AsFrames() >= vm->posInterpEndTime)
        {
            vm->posInterpEndTime = 0;
        }
        vm->posInterpTime.Tick();
    }
    vm->currentTimeInScript.Tick();
    return 0;
}

void AnmManager::DrawTextToSprite(u32 textureDstIdx, i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight,
                                  i32 fontWidth, i32 fontHeight, ZunColor textColor, ZunColor shadowColor,
                                  char *strToPrint)
{
    if (fontWidth <= 0)
    {
        fontWidth = 15;
    }
    if (fontHeight <= 0)
    {
        fontHeight = 15;
    }
    TextHelper::RenderTextToTexture(xPos, yPos, spriteWidth, spriteHeight, fontWidth, fontHeight, textColor,
                                    shadowColor, strToPrint, this->textures[textureDstIdx]);
    return;
}

#pragma var_order(argptr, buffer, fontWidth)
void AnmManager::DrawVmTextFmt(AnmManager *anmMgr, AnmVm *vm, ZunColor textColor, ZunColor shadowColor, char *fmt, ...)
{
    u32 fontWidth;
    char buffer[64];
    va_list argptr;

    fontWidth = vm->fontWidth;
    va_start(argptr, fmt);
    vsprintf(buffer, fmt, argptr);
    va_end(argptr);
    anmMgr->DrawTextToSprite(vm->sprite->sourceFileIndex, vm->sprite->startPixelInclusive.x,
                             vm->sprite->startPixelInclusive.y, vm->sprite->textureWidth, vm->sprite->textureHeight,
                             fontWidth, vm->fontHeight, textColor, shadowColor, buffer);
    vm->flags.isVisible = true;
    return;
}

#pragma var_order(args, secondPartStartX, buf, fontWidth)
void AnmManager::DrawStringFormat(AnmManager *mgr, AnmVm *vm, ZunColor textColor, ZunColor shadowColor, char *fmt, ...)
{
    char buf[64];
    va_list args;
    i32 fontWidth;
    i32 secondPartStartX;

    fontWidth = vm->fontWidth <= 0 ? 15 : vm->fontWidth;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    mgr->DrawTextToSprite(vm->sprite->sourceFileIndex, vm->sprite->startPixelInclusive.x,
                          vm->sprite->startPixelInclusive.y, vm->sprite->textureWidth, vm->sprite->textureHeight,
                          fontWidth, vm->fontHeight, textColor, shadowColor, " ");
    secondPartStartX =
        vm->sprite->startPixelInclusive.x + vm->sprite->textureWidth - ((f32)strlen(buf) * (f32)(fontWidth + 1) / 2.0f);
    mgr->DrawTextToSprite(vm->sprite->sourceFileIndex, secondPartStartX, vm->sprite->startPixelInclusive.y,
                          vm->sprite->textureWidth, vm->sprite->textureHeight, fontWidth, vm->fontHeight, textColor,
                          shadowColor, buf);
    vm->flags.isVisible = true;
    return;
}

#pragma var_order(args, secondPartStartX, buf, fontWidth)
void AnmManager::DrawStringFormat2(AnmManager *mgr, AnmVm *vm, ZunColor textColor, ZunColor shadowColor, char *fmt, ...)
{
    char buf[64];
    va_list args;
    i32 fontWidth;
    i32 secondPartStartX;

    fontWidth = vm->fontWidth <= 0 ? 15 : vm->fontWidth;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    mgr->DrawTextToSprite(vm->sprite->sourceFileIndex, vm->sprite->startPixelInclusive.x,
                          vm->sprite->startPixelInclusive.y, vm->sprite->textureWidth, vm->sprite->textureHeight,
                          fontWidth, vm->fontHeight, textColor, shadowColor, " ");
    secondPartStartX = vm->sprite->startPixelInclusive.x + vm->sprite->textureWidth / 2.0f -
                       ((f32)strlen(buf) * (f32)(fontWidth + 1) / 4.0f);
    mgr->DrawTextToSprite(vm->sprite->sourceFileIndex, secondPartStartX, vm->sprite->startPixelInclusive.y,
                          vm->sprite->textureWidth, vm->sprite->textureHeight, fontWidth, vm->fontHeight, textColor,
                          shadowColor, buf);
    vm->flags.isVisible = true;
    return;
}

ZunResult AnmManager::LoadSurface(i32 surfaceIdx, char *path)
{
    if (this->surfaces[surfaceIdx] != 0)
    {
        this->ReleaseSurface(surfaceIdx);
    }
    u8 *data = FileSystem::OpenPath(path, 0);
    if (data == NULL)
    {
        GameErrorContext::Fatal(&g_GameErrorContext, TH_ERR_CANNOT_BE_LOADED, path);
        return ZUN_ERROR;
    }

    u32 tex = g_Renderer->LoadSurfaceFromFile(data, g_LastFileSize, &this->surfaceSourceInfo[surfaceIdx]);
    if (tex == 0)
    {
        free(data);
        return ZUN_ERROR;
    }

    this->surfaces[surfaceIdx] = tex;
    this->surfacesBis[surfaceIdx] = tex;

    free(data);
    return ZUN_SUCCESS;
}

void AnmManager::ReleaseSurface(i32 surfaceIdx)
{
    if (this->surfaces[surfaceIdx] != 0)
    {
        g_Renderer->DeleteTexture(this->surfaces[surfaceIdx]);
        this->surfaces[surfaceIdx] = 0;
    }
    this->surfacesBis[surfaceIdx] = 0;
}

void AnmManager::CopySurfaceToBackBuffer(i32 surfaceIdx, i32 left, i32 top, i32 x, i32 y)
{
    if (this->surfacesBis[surfaceIdx] == 0)
    {
        return;
    }

    g_Renderer->CopySurfaceToScreen(this->surfaces[surfaceIdx],
                                   this->surfaceSourceInfo[surfaceIdx].Width,
                                   this->surfaceSourceInfo[surfaceIdx].Height,
                                   x, y);
}

void AnmManager::DrawEndingRect(i32 surfaceIdx, i32 rectX, i32 rectY, i32 rectLeft, i32 rectTop, i32 width, i32 height)
{
    if (this->surfacesBis[surfaceIdx] == 0)
    {
        return;
    }

    g_Renderer->CopySurfaceRectToScreen(this->surfaces[surfaceIdx],
                                       rectLeft, rectTop, width, height,
                                       rectX, rectY,
                                       this->surfaceSourceInfo[surfaceIdx].Width,
                                       this->surfaceSourceInfo[surfaceIdx].Height);
}

#pragma var_order(rect, destSurface, sourceSurface)
void AnmManager::TakeScreenshot(i32 textureId, i32 left, i32 top, i32 width, i32 height)
{
    if (this->textures[textureId] == 0)
    {
        return;
    }
    g_Renderer->TakeScreenshot(this->textures[textureId], left, top, width, height);
    return;
}
}; // namespace th06
