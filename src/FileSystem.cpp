#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "AssetIO.hpp"
#include "FileSystem.hpp"
#include "GamePaths.hpp"
#include "pbg3/Pbg3Archive.hpp"
#include "utils.hpp"

namespace th06
{
DIFFABLE_STATIC(u32, g_LastFileSize)

namespace
{
bool TryReadStandaloneArchiveEntry(const char *archivePath, const char *entryname, u8 **outData, u32 *outSize)
{
    if (archivePath == NULL || entryname == NULL || outData == NULL || outSize == NULL)
    {
        return false;
    }

    Pbg3Archive archive;
    if (archive.Load((char *)archivePath) == FALSE)
    {
        return false;
    }

    const i32 entryIdx = archive.FindEntry((char *)entryname);
    if (entryIdx < 0)
    {
        return false;
    }

    u8 *data = archive.ReadDecompressEntry(entryIdx, (char *)entryname);
    if (data == NULL)
    {
        return false;
    }

    *outData = data;
    *outSize = archive.GetEntrySize(entryIdx);
    return true;
}

u8 *TryReadModifiedCmArchiveEntry(const char *entryname)
{
    static const char *kModifiedCmArchives[] = {
        "TOLOL_CM.dat",
        "TOTOL_CM.dat",
        "TOLOL_CM.DAT",
        "TOTOL_CM.DAT",
        "tolol_cm.dat",
        "totol_cm.dat",
    };

    for (const char *archivePath : kModifiedCmArchives)
    {
        u8 *data = NULL;
        u32 size = 0;
        if (TryReadStandaloneArchiveEntry(archivePath, entryname, &data, &size))
        {
            g_LastFileSize = size;
            utils::DebugPrint2("%s Decode from %s ... \n", entryname, archivePath);
            return data;
        }
    }

    return NULL;
}
} // namespace

#pragma var_order(pbg3Idx, entryname, entryIdx, fsize, data, file)
u8 *FileSystem::OpenPath(char *filepath, int isExternalResource)
{
    u8 *data;
    FILE *file;
    size_t fsize;
    i32 entryIdx;
    char *entryname;
    i32 pbg3Idx;

    // Resolve platform-specific path (Android: assets vs user data).
    char resolvedPath[512];
    GamePaths::Resolve(resolvedPath, sizeof(resolvedPath), filepath);

    entryIdx = -1;
    if (isExternalResource == 0)
    {
        entryname = strrchr(filepath, '\\');
        if (entryname == (char *)0x0)
        {
            entryname = filepath;
        }
        else
        {
            entryname = entryname + 1;
        }
        entryname = strrchr(entryname, '/');
        if (entryname == (char *)0x0)
        {
            entryname = filepath;
        }
        else
        {
            entryname = entryname + 1;
        }
        if (g_Pbg3Archives != NULL)
        {
            for (pbg3Idx = 0; pbg3Idx < 0x10; pbg3Idx += 1)
            {
                if (g_Pbg3Archives[pbg3Idx] != NULL)
                {
                    entryIdx = g_Pbg3Archives[pbg3Idx]->FindEntry(entryname);
                    if (entryIdx >= 0)
                    {
                        break;
                    }
                }
            }
        }
        if (entryIdx < 0)
        {
            data = TryReadModifiedCmArchiveEntry(entryname);
            if (data != NULL)
            {
                return data;
            }
            return NULL;
        }
    }
    if (entryIdx >= 0)
    {
        utils::DebugPrint2("%s Decode ... \n", entryname);
        data = g_Pbg3Archives[pbg3Idx]->ReadDecompressEntry(entryIdx, entryname);
        g_LastFileSize = g_Pbg3Archives[pbg3Idx]->GetEntrySize(entryIdx);
    }
    else
    {
        utils::DebugPrint2("%s Load ... \n", resolvedPath);
        // Routed through AssetIO so the same loader works on every platform:
        //   - desktop: searches cwd then exe-dir-relative
        //   - Android: SDL_RWFromFile reads APK assets via AAssetManager
        size_t fileSize = 0;
        data = AssetIO::ReadAll(resolvedPath, &fileSize);
        if (data == NULL)
        {
            utils::DebugPrint2("error : %s is not found.\n", resolvedPath);
            return NULL;
        }
        g_LastFileSize = (u32)fileSize;
    }
    return data;
}

int FileSystem::WriteDataToFile(char *path, void *data, size_t size)
{
    FILE *f;

    // Resolve to writable user-data directory on Android.
    char resolvedPath[512];
    GamePaths::Resolve(resolvedPath, sizeof(resolvedPath), path);
    GamePaths::EnsureParentDir(resolvedPath);

    f = fopen(resolvedPath, "wb");
    if (f == NULL)
    {
        AssetIO::DiagLog("FS", "WriteDataToFile FAIL fopen path=%s resolved=%s size=%zu errno=%d",
                         path ? path : "(null)", resolvedPath, size, errno);
        return -1;
    }
    else
    {
        if (fwrite(data, 1, size, f) != size)
        {
            AssetIO::DiagLog("FS", "WriteDataToFile FAIL fwrite path=%s resolved=%s size=%zu",
                             path ? path : "(null)", resolvedPath, size);
            fclose(f);
            return -2;
        }
        else
        {
            fclose(f);
            AssetIO::DiagLog("FS", "WriteDataToFile OK path=%s resolved=%s size=%zu",
                             path ? path : "(null)", resolvedPath, size);
            return 0;
        }
    }
}
}; // namespace th06
