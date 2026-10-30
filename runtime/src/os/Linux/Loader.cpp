// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "os/Loader.h"

#include <dlfcn.h>
#if defined(__ANDROID__)
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#endif

#include "Base/Log.h"
namespace MapleRuntime {
namespace Os {
void* Loader::LoadBinaryFile(const char* path)
{
    if (path == nullptr) {
        return nullptr;
    }
    void* handler = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handler == nullptr) {
        LOG(RTLOG_ERROR, dlerror());
    }
    return handler;
}

int Loader::UnloadBinaryFile(void* handler)
{
    if (handler == nullptr) {
        return -1;
    }
    int ret = dlclose(handler);
    return ret;
}

void* Loader::FindSymbol(void* handler, const char* symbolName)
{
    if (symbolName == nullptr) {
        return nullptr;
    }
    // handler can be nullptr
    return dlsym(handler, symbolName);
}

int Loader::GetBinaryInfoFromAddress(const void* address, BinaryInfo* binInfo)
{
    if (address == nullptr) {
        return 0;
    }
    Dl_info dlInfo;
    int ret = dladdr(address, &dlInfo);
    if (!ret) {
        return 0;
    }
    binInfo->filePathName = const_cast<char*>(dlInfo.dli_fname);
    binInfo->symbolName = const_cast<char*>(dlInfo.dli_sname);
    return ret;
}
#if defined(__ANDROID__)
namespace {
constexpr size_t kModuleProbeSize = 4096;
constexpr size_t kMaxModulePathLen = 2048;
constexpr uint64_t kMaxCentralDirectorySize = 32ull * 1024ull * 1024ull;
constexpr uint64_t kMaxCentralDirectoryEntries = 200000;
constexpr size_t kMaxContainerCacheEntries = 8;

struct ModuleImage {
    const uint8_t* start = nullptr;
    size_t fileBytes = 0;
};

struct ImageSearchCtx {
    uintptr_t target = 0;
    ModuleImage image;
};

struct ContainerEntry {
    std::string name;
    uint64_t localHeaderOffset = 0;
    uint64_t uncompressedSize = 0;
};

struct ContainerIndex {
    dev_t dev = 0;
    ino_t ino = 0;
    uint64_t size = 0;
    time_t mtime = 0;
    std::vector<ContainerEntry> entries;
};

std::mutex gContainerCacheMutex;
std::unordered_map<std::string, std::shared_ptr<ContainerIndex>> gContainerCache;

uint16_t ReadLe16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t ReadLe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadLe64(const uint8_t* p)
{
    return static_cast<uint64_t>(ReadLe32(p)) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32);
}

bool PreadExact(int fd, void* buffer, size_t length, uint64_t offset)
{
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, out + done, length - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

int FindImageCallback(struct dl_phdr_info* info, size_t, void* data)
{
    ImageSearchCtx* ctx = static_cast<ImageSearchCtx*>(data);
    if (ctx == nullptr || info == nullptr || info->dlpi_phdr == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < static_cast<size_t>(info->dlpi_phnum); ++i) {
        const ElfW(Phdr)* ph = info->dlpi_phdr + i;
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr) + static_cast<uintptr_t>(ph->p_vaddr);
        uintptr_t end = start + static_cast<uintptr_t>(ph->p_memsz);
        if (end < start || ctx->target < start || ctx->target >= end) {
            continue;
        }
        // The module containing the metadata address was found. Locate the segment mapped
        // from file offset 0; only that segment can be compared with the file header.
        for (size_t j = 0; j < static_cast<size_t>(info->dlpi_phnum); ++j) {
            const ElfW(Phdr)* first = info->dlpi_phdr + j;
            if (first->p_type != PT_LOAD || first->p_offset != 0) {
                continue;
            }
            if ((first->p_flags & PF_R) == 0 || first->p_filesz < sizeof(ElfW(Ehdr))) {
                return 1;
            }
            uintptr_t imageStart =
                static_cast<uintptr_t>(info->dlpi_addr) + static_cast<uintptr_t>(first->p_vaddr);
            ctx->image.start = reinterpret_cast<const uint8_t*>(imageStart);
            ctx->image.fileBytes = static_cast<size_t>(first->p_filesz);
            return 1;
        }
        return 1;
    }
    return 0;
}

bool ResolveModuleImage(const void* address, ModuleImage* out)
{
    if (address == nullptr || out == nullptr) {
        return false;
    }
    ImageSearchCtx ctx;
    ctx.target = reinterpret_cast<uintptr_t>(address);
    dl_iterate_phdr(FindImageCallback, &ctx);
    if (ctx.image.start == nullptr || ctx.image.fileBytes < sizeof(ElfW(Ehdr))) {
        return false;
    }
    *out = ctx.image;
    return true;
}

bool IsSafeEntryName(const std::string& name)
{
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos ||
        name.find('\0') != std::string::npos || name.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool ParseZip64Extra(const uint8_t* extra, size_t extraLen, bool needUncompressed, bool needCompressed,
    bool needOffset, bool needDisk, uint64_t* uncompressed, uint64_t* compressed, uint64_t* localOffset,
    uint32_t* diskStart)
{
    size_t pos = 0;
    while (pos + 4 <= extraLen) {
        uint16_t fieldId = ReadLe16(extra + pos);
        uint16_t fieldSize = ReadLe16(extra + pos + 2);
        pos += 4;
        if (pos + fieldSize > extraLen) {
            return false;
        }
        if (fieldId == 0x0001) {
            size_t cur = pos;
            size_t end = pos + fieldSize;
            if (needUncompressed) {
                if (cur + 8 > end) {
                    return false;
                }
                *uncompressed = ReadLe64(extra + cur);
                cur += 8;
            }
            if (needCompressed) {
                if (cur + 8 > end) {
                    return false;
                }
                *compressed = ReadLe64(extra + cur);
                cur += 8;
            }
            if (needOffset) {
                if (cur + 8 > end) {
                    return false;
                }
                *localOffset = ReadLe64(extra + cur);
                cur += 8;
            }
            if (needDisk) {
                if (cur + 4 > end) {
                    return false;
                }
                *diskStart = ReadLe32(extra + cur);
                cur += 4;
            }
            return true;
        }
        pos += fieldSize;
    }
    return false;
}

bool LoadCentralDirectory(int fd, uint64_t fileSize, std::vector<ContainerEntry>* entries)
{
    if (entries == nullptr || fileSize < 22) {
        return false;
    }
    const uint64_t kTailMax = 22 + 65535;
    uint64_t tailSize64 = std::min(fileSize, kTailMax);
    size_t tailSize = static_cast<size_t>(tailSize64);
    std::vector<uint8_t> tail(tailSize);
    if (!PreadExact(fd, tail.data(), tailSize, fileSize - tailSize64)) {
        return false;
    }
    int64_t eocdRel = -1;
    for (int64_t i = static_cast<int64_t>(tailSize) - 22; i >= 0; --i) {
        if (ReadLe32(tail.data() + static_cast<size_t>(i)) != 0x06054b50) {
            continue;
        }
        uint16_t commentLen = ReadLe16(tail.data() + static_cast<size_t>(i) + 20);
        if (static_cast<uint64_t>(i) + 22 + commentLen <= tailSize64) {
            eocdRel = i;
            break;
        }
    }
    if (eocdRel < 0) {
        return false;
    }
    uint64_t eocdOffset = fileSize - tailSize64 + static_cast<uint64_t>(eocdRel);
    uint16_t entryCount16 = ReadLe16(tail.data() + static_cast<size_t>(eocdRel) + 10);
    uint32_t cdSize32 = ReadLe32(tail.data() + static_cast<size_t>(eocdRel) + 12);
    uint32_t cdOffset32 = ReadLe32(tail.data() + static_cast<size_t>(eocdRel) + 16);
    uint64_t entryCount = entryCount16;
    uint64_t cdSize = cdSize32;
    uint64_t cdOffset = cdOffset32;

    if (entryCount16 == 0xffff || cdSize32 == 0xffffffff || cdOffset32 == 0xffffffff) {
        if (eocdOffset < 20) {
            return false;
        }
        uint8_t locator[20];
        if (!PreadExact(fd, locator, sizeof(locator), eocdOffset - 20) ||
            ReadLe32(locator) != 0x07064b50) {
            return false;
        }
        uint64_t zip64Offset = ReadLe64(locator + 8);
        uint8_t record[56];
        if (zip64Offset > fileSize || fileSize - zip64Offset < sizeof(record) ||
            !PreadExact(fd, record, sizeof(record), zip64Offset) ||
            ReadLe32(record) != 0x06064b50) {
            return false;
        }
        entryCount = ReadLe64(record + 32);
        cdSize = ReadLe64(record + 40);
        cdOffset = ReadLe64(record + 48);
    }

    if (entryCount == 0 || entryCount > kMaxCentralDirectoryEntries || cdSize == 0 ||
        cdSize > kMaxCentralDirectorySize || cdOffset > fileSize || cdSize > fileSize - cdOffset) {
        return false;
    }
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (!PreadExact(fd, cd.data(), cd.size(), cdOffset)) {
        return false;
    }

    entries->clear();
    entries->reserve(static_cast<size_t>(std::min<uint64_t>(entryCount, 4096)));
    size_t pos = 0;
    uint64_t parsed = 0;
    while (pos + 46 <= cd.size() && parsed < entryCount) {
        const uint8_t* p = cd.data() + pos;
        if (ReadLe32(p) != 0x02014b50) {
            return false;
        }
        uint16_t flags = ReadLe16(p + 8);
        uint16_t method = ReadLe16(p + 10);
        uint64_t compressed = ReadLe32(p + 20);
        uint64_t uncompressed = ReadLe32(p + 24);
        uint16_t nameLen = ReadLe16(p + 28);
        uint16_t extraLen = ReadLe16(p + 30);
        uint16_t commentLen = ReadLe16(p + 32);
        uint16_t disk16 = ReadLe16(p + 34);
        uint64_t localOffset = ReadLe32(p + 42);
        uint64_t total = 46ull + nameLen + extraLen + commentLen;
        if (total > static_cast<uint64_t>(cd.size() - pos)) {
            return false;
        }

        bool needUncompressed = uncompressed == 0xffffffff;
        bool needCompressed = compressed == 0xffffffff;
        bool needOffset = localOffset == 0xffffffff;
        bool needDisk = disk16 == 0xffff;
        if (needUncompressed || needCompressed || needOffset || needDisk) {
            uint32_t disk32 = disk16;
            if (!ParseZip64Extra(p + 46 + nameLen, extraLen, needUncompressed, needCompressed, needOffset,
                needDisk, &uncompressed, &compressed, &localOffset, &disk32)) {
                pos += static_cast<size_t>(total);
                ++parsed;
                continue;
            }
            if (needDisk && disk32 != 0) {
                pos += static_cast<size_t>(total);
                ++parsed;
                continue;
            }
        }
        if (!needDisk && disk16 != 0) {
            pos += static_cast<size_t>(total);
            ++parsed;
            continue;
        }

        if ((flags & 0x0001u) == 0 && method == 0 && nameLen > 0 && uncompressed > 0) {
            std::string name(reinterpret_cast<const char*>(p + 46), nameLen);
            if (IsSafeEntryName(name) && name.back() != '/') {
                ContainerEntry entry;
                entry.name = name;
                entry.localHeaderOffset = localOffset;
                entry.uncompressedSize = uncompressed;
                entries->push_back(entry);
            }
        }
        pos += static_cast<size_t>(total);
        ++parsed;
    }
    return parsed == entryCount;
}

bool GetContainerIndex(const char* path, int fd, const struct stat& st,
    std::shared_ptr<const ContainerIndex>* out)
{
    if (path == nullptr || out == nullptr || st.st_size < 0) {
        return false;
    }
    std::string key(path);
    uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    {
        std::lock_guard<std::mutex> lock(gContainerCacheMutex);
        auto it = gContainerCache.find(key);
        if (it != gContainerCache.end() && it->second->dev == st.st_dev && it->second->ino == st.st_ino &&
            it->second->size == fileSize && it->second->mtime == st.st_mtime) {
            *out = it->second;
            return true;
        }
    }

    std::shared_ptr<ContainerIndex> index = std::make_shared<ContainerIndex>();
    index->dev = st.st_dev;
    index->ino = st.st_ino;
    index->size = fileSize;
    index->mtime = st.st_mtime;
    if (!LoadCentralDirectory(fd, fileSize, &index->entries) || index->entries.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(gContainerCacheMutex);
        auto it = gContainerCache.find(key);
        if (it != gContainerCache.end() && it->second->dev == index->dev &&
            it->second->ino == index->ino && it->second->size == index->size &&
            it->second->mtime == index->mtime && !it->second->entries.empty()) {
            *out = it->second;
            return true;
        }
        if (gContainerCache.size() >= kMaxContainerCacheEntries) {
            gContainerCache.erase(gContainerCache.begin());
        }
        gContainerCache[key] = index;
    }
    *out = index;
    return true;
}

bool FilePrefixMatchesImage(const char* path, const ModuleImage& image, size_t probeLen)
{
    if (path == nullptr || path[0] != '/' || image.start == nullptr || probeLen == 0 ||
        probeLen > kModuleProbeSize) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    uint8_t buffer[kModuleProbeSize];
    bool same = false;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        static_cast<uint64_t>(st.st_size) >= probeLen && PreadExact(fd, buffer, probeLen, 0)) {
        same = memcmp(buffer, image.start, probeLen) == 0;
    }
    close(fd);
    return same;
}

bool FindContainerEntry(const char* path, const ModuleImage& image, size_t probeLen, std::string* entryName)
{
    if (path == nullptr || entryName == nullptr || probeLen == 0 || probeLen > kModuleProbeSize) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    bool found = false;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        std::shared_ptr<const ContainerIndex> index;
        if (GetContainerIndex(path, fd, st, &index)) {
            std::vector<std::string> matches;
            uint8_t buffer[kModuleProbeSize];
            uint64_t fileSize = static_cast<uint64_t>(st.st_size);
            for (size_t i = 0; i < index->entries.size(); ++i) {
                const ContainerEntry& entry = index->entries[i];
                if (entry.uncompressedSize < probeLen) {
                    continue;
                }
                uint8_t localHeader[30];
                const uint64_t kLocalHeaderSize = static_cast<uint64_t>(sizeof(localHeader));
                if (entry.localHeaderOffset > fileSize || kLocalHeaderSize > fileSize - entry.localHeaderOffset) {
                    continue;
                }
                if (!PreadExact(fd, localHeader, static_cast<size_t>(kLocalHeaderSize), entry.localHeaderOffset) ||
                    ReadLe32(localHeader) != 0x04034b50) {
                    continue;
                }
                uint16_t localNameLen = ReadLe16(localHeader + 26);
                uint16_t localExtraLen = ReadLe16(localHeader + 28);
                uint64_t afterHeader = entry.localHeaderOffset + kLocalHeaderSize;
                if (static_cast<uint64_t>(localNameLen) + static_cast<uint64_t>(localExtraLen) >
                    fileSize - afterHeader) {
                    continue;
                }
                uint64_t dataOffset =
                    afterHeader + static_cast<uint64_t>(localNameLen) + static_cast<uint64_t>(localExtraLen);
                if (dataOffset > fileSize || static_cast<uint64_t>(probeLen) > fileSize - dataOffset) {
                    continue;
                }
                if (!PreadExact(fd, buffer, probeLen, dataOffset)) {
                    continue;
                }
                if (memcmp(buffer, ELFMAG, SELFMAG) != 0 || memcmp(buffer, image.start, probeLen) != 0) {
                    continue;
                }
                matches.push_back(entry.name);
            }
            if (matches.size() == 1) {
                *entryName = matches[0];
                found = true;
            } else if (matches.size() > 1) {
                LOG(RTLOG_WARNING, "[CJRT] module source: multiple entries match path=%s matches=%u", path,
                    static_cast<unsigned>(matches.size()));
            }
        }
    }
    close(fd);
    return found;
}
} // namespace
#endif

#if defined(__ANDROID__)
void Loader::CanonicalizeModulePath(const void* address, BinaryInfo* binInfo)
{
    if (address == nullptr || binInfo == nullptr || binInfo->filePathName.IsEmpty()) {
        return;
    }
    const char* reported = binInfo->filePathName.Str();
    if (reported == nullptr || reported[0] != '/') {
        return;
    }
    if (strstr(reported, "!/") != nullptr) {
        LOG(RTLOG_DEBUG, "[CJRT] module source: branch=already-canonical path=%s", reported);
        return;
    }
    // Keep a copy: assigning binInfo->filePathName below overwrites the FixedCString
    // buffer that "reported" points into.
    std::string reportedPath(reported);

    ModuleImage image;
    if (!ResolveModuleImage(address, &image)) {
        LOG(RTLOG_DEBUG, "[CJRT] module source: branch=fallback path=%s reason=no_image", reportedPath.c_str());
        return;
    }
    size_t probeLen =
        static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(kModuleProbeSize),
            static_cast<uint64_t>(image.fileBytes)));
    if (probeLen < sizeof(ElfW(Ehdr))) {
        LOG(RTLOG_DEBUG, "[CJRT] module source: branch=fallback path=%s reason=image_too_small", reportedPath.c_str());
        return;
    }

    if (FilePrefixMatchesImage(reportedPath.c_str(), image, probeLen)) {
        LOG(RTLOG_DEBUG, "[CJRT] module source: branch=direct path=%s", reportedPath.c_str());
        return;
    }

    std::string entryName;
    if (!FindContainerEntry(reportedPath.c_str(), image, probeLen, &entryName)) {
        LOG(RTLOG_INFO, "[CJRT] module source: branch=fallback (no change) path=%s reason=no_unique_entry",
            reportedPath.c_str());
        return;
    }

    size_t reportedLen = reportedPath.size();
    if (reportedLen > kMaxModulePathLen || entryName.size() > kMaxModulePathLen ||
        reportedLen + 2 + entryName.size() >= kMaxModulePathLen) {
        LOG(RTLOG_INFO, "[CJRT] module source: branch=fallback (no change) path=%s reason=too_long",
            reportedPath.c_str());
        return;
    }
    std::string canonical;
    canonical.reserve(reportedLen + 2 + entryName.size());
    canonical.append(reportedPath);
    canonical.append("!/");
    canonical.append(entryName);
    binInfo->filePathName = FixedCString(canonical.c_str());
    LOG(RTLOG_INFO, "[CJRT] module source: branch=container %s -> %s (probe=%u)", reportedPath.c_str(),
        canonical.c_str(), static_cast<unsigned>(probeLen));
}
#endif

} // namespace Os
} // namespace MapleRuntime
