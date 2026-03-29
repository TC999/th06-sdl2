#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "inttypes.hpp"

namespace th06::PortableSnapshotStorage
{
struct DiskSnapshotStats
{
    size_t rawBytes = 0;
    size_t storedBytes = 0;
    bool isCompressed = false;
};

bool EncodePortableSnapshotForDisk(const std::vector<u8> &rawBytes, std::vector<u8> &diskBytes,
                                   DiskSnapshotStats *outStats = nullptr, std::string *outError = nullptr);
bool DecodePortableSnapshotFromDisk(const std::vector<u8> &diskBytes, std::vector<u8> &rawBytes,
                                    DiskSnapshotStats *outStats = nullptr, std::string *outError = nullptr);
bool CompressPortableSnapshotForTransport(const std::vector<u8> &rawBytes, std::vector<u8> &compressedBytes,
                                          std::string *outError = nullptr);
bool DecompressPortableSnapshotFromTransport(const std::vector<u8> &compressedBytes, size_t expectedRawBytes,
                                             std::vector<u8> &rawBytes, std::string *outError = nullptr);
} // namespace th06::PortableSnapshotStorage
