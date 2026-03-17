#include <stdio.h>
#include <string.h>

#include "FileSystem.hpp"
#include "GamePaths.hpp"
#include "pbg3/Pbg3Archive.hpp"
#include "utils.hpp"
#ifdef __ANDROID__
#include <SDL.h>
#endif

namespace th06
{
DIFFABLE_STATIC(u32, g_LastFileSize)

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
#ifdef __ANDROID__
        // On Android, use SDL_RWFromFile to transparently read from APK assets.
        SDL_RWops *rw = SDL_RWFromFile(resolvedPath, "rb");
        if (rw == NULL)
        {
            utils::DebugPrint2("error : %s is not found.\n", resolvedPath);
            return NULL;
        }
        else
        {
            i64 rwSize = SDL_RWsize(rw);
            fsize = (rwSize > 0) ? (size_t)rwSize : 0;
            g_LastFileSize = fsize;
            data = (u8 *)malloc(fsize);
            SDL_RWread(rw, data, 1, fsize);
            SDL_RWclose(rw);
        }
#else
        file = fopen(resolvedPath, "rb");
        if (file == NULL)
        {
            utils::DebugPrint2("error : %s is not found.\n", resolvedPath);
            return NULL;
        }
        else
        {
            fseek(file, 0, SEEK_END);
            fsize = ftell(file);
            g_LastFileSize = fsize;
            fseek(file, 0, SEEK_SET);
            data = (u8 *)malloc(fsize);
            fread(data, 1, fsize, file);
            fclose(file);
        }
#endif
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
        return -1;
    }
    else
    {
        if (fwrite(data, 1, size, f) != size)
        {
            fclose(f);
            return -2;
        }
        else
        {
            fclose(f);
            return 0;
        }
    }
}
}; // namespace th06
