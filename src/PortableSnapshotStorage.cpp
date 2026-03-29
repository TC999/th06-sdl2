#include "PortableSnapshotStorage.hpp"

#include <cstring>
#include "zstd.h"

namespace th06::PortableSnapshotStorage
{
namespace
{
constexpr u8 kPortableSnapshotDiskMagic[4] = {'T', 'P', 'S', 'Z'};
constexpr u32 kPortableSnapshotDiskVersion = 1;
constexpr u64 kPortableSnapshotDiskMaxRawBytes = 128ull * 1024ull * 1024ull;
constexpr int kPortableSnapshotDiskZstdLevel = 3;
constexpr int kPortableSnapshotTransportZstdLevel = 1;
constexpr size_t kPortableSnapshotDiskHeaderSize = 4 + 4 + 8 + 8;

void SetError(std::string *outError, const char *message)
{
    if (outError != nullptr)
    {
        *outError = message != nullptr ? message : "";
    }
}

void SetStats(DiskSnapshotStats *outStats, size_t rawBytes, size_t storedBytes, bool isCompressed)
{
    if (outStats != nullptr)
    {
        outStats->rawBytes = rawBytes;
        outStats->storedBytes = storedBytes;
        outStats->isCompressed = isCompressed;
    }
}

void AppendU32LE(std::vector<u8> &bytes, u32 value)
{
    bytes.push_back((u8)(value & 0xff));
    bytes.push_back((u8)((value >> 8) & 0xff));
    bytes.push_back((u8)((value >> 16) & 0xff));
    bytes.push_back((u8)((value >> 24) & 0xff));
}

void AppendU64LE(std::vector<u8> &bytes, u64 value)
{
    for (int i = 0; i < 8; ++i)
    {
        bytes.push_back((u8)((value >> (i * 8)) & 0xff));
    }
}

bool ReadU32LE(const std::vector<u8> &bytes, size_t &offset, u32 &value)
{
    if (offset + 4 > bytes.size())
    {
        return false;
    }

    value = (u32)bytes[offset] | ((u32)bytes[offset + 1] << 8) | ((u32)bytes[offset + 2] << 16) |
            ((u32)bytes[offset + 3] << 24);
    offset += 4;
    return true;
}

bool ReadU64LE(const std::vector<u8> &bytes, size_t &offset, u64 &value)
{
    if (offset + 8 > bytes.size())
    {
        return false;
    }

    value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value |= (u64)bytes[offset + i] << (i * 8);
    }
    offset += 8;
    return true;
}

bool HasPortableSnapshotDiskHeader(const std::vector<u8> &bytes)
{
    return bytes.size() >= sizeof(kPortableSnapshotDiskMagic) &&
           std::memcmp(bytes.data(), kPortableSnapshotDiskMagic, sizeof(kPortableSnapshotDiskMagic)) == 0;
}
} // namespace

bool EncodePortableSnapshotForDisk(const std::vector<u8> &rawBytes, std::vector<u8> &diskBytes,
                                   DiskSnapshotStats *outStats, std::string *outError)
{
    diskBytes.clear();
    SetStats(outStats, 0, 0, false);

    if (rawBytes.empty())
    {
        SetError(outError, "empty portable snapshot");
        return false;
    }

    if (rawBytes.size() > kPortableSnapshotDiskMaxRawBytes)
    {
        SetError(outError, "portable snapshot too large for disk envelope");
        return false;
    }

    const size_t compressedBound = ZSTD_compressBound(rawBytes.size());
    if (compressedBound == 0 || compressedBound < rawBytes.size())
    {
        SetError(outError, "invalid zstd compression bound");
        return false;
    }

    std::vector<u8> compressedBytes(compressedBound);
    const size_t compressedSize = ZSTD_compress(compressedBytes.data(), compressedBytes.size(), rawBytes.data(),
                                                rawBytes.size(), kPortableSnapshotDiskZstdLevel);
    if (ZSTD_isError(compressedSize))
    {
        SetError(outError, ZSTD_getErrorName(compressedSize));
        return false;
    }

    compressedBytes.resize(compressedSize);
    diskBytes.reserve(kPortableSnapshotDiskHeaderSize + compressedBytes.size());
    diskBytes.insert(diskBytes.end(), kPortableSnapshotDiskMagic,
                     kPortableSnapshotDiskMagic + sizeof(kPortableSnapshotDiskMagic));
    AppendU32LE(diskBytes, kPortableSnapshotDiskVersion);
    AppendU64LE(diskBytes, (u64)rawBytes.size());
    AppendU64LE(diskBytes, (u64)compressedBytes.size());
    diskBytes.insert(diskBytes.end(), compressedBytes.begin(), compressedBytes.end());

    SetStats(outStats, rawBytes.size(), diskBytes.size(), true);
    SetError(outError, nullptr);
    return true;
}

bool DecodePortableSnapshotFromDisk(const std::vector<u8> &diskBytes, std::vector<u8> &rawBytes,
                                    DiskSnapshotStats *outStats, std::string *outError)
{
    rawBytes.clear();
    SetStats(outStats, 0, 0, false);

    if (diskBytes.empty())
    {
        SetError(outError, "empty portable snapshot file");
        return false;
    }

    if (!HasPortableSnapshotDiskHeader(diskBytes))
    {
        rawBytes = diskBytes;
        SetStats(outStats, rawBytes.size(), diskBytes.size(), false);
        SetError(outError, nullptr);
        return true;
    }

    size_t offset = sizeof(kPortableSnapshotDiskMagic);
    u32 version = 0;
    u64 rawSize = 0;
    u64 compressedSize = 0;
    if (!ReadU32LE(diskBytes, offset, version) || !ReadU64LE(diskBytes, offset, rawSize) ||
        !ReadU64LE(diskBytes, offset, compressedSize))
    {
        SetError(outError, "portable snapshot header truncated");
        return false;
    }

    if (version != kPortableSnapshotDiskVersion)
    {
        SetError(outError, "portable snapshot disk version unsupported");
        return false;
    }
    if (rawSize == 0 || rawSize > kPortableSnapshotDiskMaxRawBytes)
    {
        SetError(outError, "portable snapshot raw size invalid");
        return false;
    }
    if (compressedSize == 0 || compressedSize > diskBytes.size() || offset + (size_t)compressedSize != diskBytes.size())
    {
        SetError(outError, "portable snapshot compressed size invalid");
        return false;
    }

    rawBytes.resize((size_t)rawSize);
    const size_t decompressedSize =
        ZSTD_decompress(rawBytes.data(), rawBytes.size(), diskBytes.data() + offset, (size_t)compressedSize);
    if (ZSTD_isError(decompressedSize))
    {
        rawBytes.clear();
        SetError(outError, ZSTD_getErrorName(decompressedSize));
        return false;
    }
    if (decompressedSize != rawBytes.size())
    {
        rawBytes.clear();
        SetError(outError, "portable snapshot decompressed size mismatch");
        return false;
    }

    SetStats(outStats, rawBytes.size(), diskBytes.size(), true);
    SetError(outError, nullptr);
    return true;
}

bool CompressPortableSnapshotForTransport(const std::vector<u8> &rawBytes, std::vector<u8> &compressedBytes,
                                          std::string *outError)
{
    compressedBytes.clear();

    if (rawBytes.empty())
    {
        SetError(outError, "empty portable snapshot");
        return false;
    }

    if (rawBytes.size() > kPortableSnapshotDiskMaxRawBytes)
    {
        SetError(outError, "portable snapshot too large for transport envelope");
        return false;
    }

    const size_t compressedBound = ZSTD_compressBound(rawBytes.size());
    if (compressedBound == 0 || compressedBound < rawBytes.size())
    {
        SetError(outError, "invalid zstd compression bound");
        return false;
    }

    compressedBytes.resize(compressedBound);
    const size_t compressedSize = ZSTD_compress(compressedBytes.data(), compressedBytes.size(), rawBytes.data(),
                                                rawBytes.size(), kPortableSnapshotTransportZstdLevel);
    if (ZSTD_isError(compressedSize))
    {
        compressedBytes.clear();
        SetError(outError, ZSTD_getErrorName(compressedSize));
        return false;
    }

    compressedBytes.resize(compressedSize);
    SetError(outError, nullptr);
    return true;
}

bool DecompressPortableSnapshotFromTransport(const std::vector<u8> &compressedBytes, size_t expectedRawBytes,
                                             std::vector<u8> &rawBytes, std::string *outError)
{
    rawBytes.clear();

    if (compressedBytes.empty())
    {
        SetError(outError, "empty compressed portable snapshot");
        return false;
    }
    if (expectedRawBytes == 0 || expectedRawBytes > kPortableSnapshotDiskMaxRawBytes)
    {
        SetError(outError, "portable snapshot raw size invalid");
        return false;
    }

    rawBytes.resize(expectedRawBytes);
    const size_t decompressedSize =
        ZSTD_decompress(rawBytes.data(), rawBytes.size(), compressedBytes.data(), compressedBytes.size());
    if (ZSTD_isError(decompressedSize))
    {
        rawBytes.clear();
        SetError(outError, ZSTD_getErrorName(decompressedSize));
        return false;
    }
    if (decompressedSize != rawBytes.size())
    {
        rawBytes.clear();
        SetError(outError, "portable snapshot decompressed size mismatch");
        return false;
    }

    SetError(outError, nullptr);
    return true;
}
} // namespace th06::PortableSnapshotStorage
