#pragma once

#undef _DLL

#include <string>

#include <stdint.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <unordered_set>
#include "Resource.h"
#include <StormLib.h>
#include <mutex>

struct StormFile;

class StormArchive : public std::enable_shared_from_this<StormArchive> {
  public:
    StormArchive(const std::string& mainPath, bool enableWriting);
    StormArchive(const std::string& mainPath, const std::string& patchesPath,
            const std::unordered_set<uint32_t>& validHashes, bool enableWriting, bool generateCrcMap = true);
    StormArchive(const std::vector<std::string>& fileList, const std::unordered_set<uint32_t>& validHashes,
            bool enableWriting, bool generateCrcMap = true);
    ~StormArchive();

    static std::shared_ptr<StormArchive> CreateArchive(const std::string& archivePath, size_t fileCapacity);

    bool IsMainMPQValid();
    std::shared_ptr<StormFile> LoadFile(const std::string& filePath, bool includeParent = true);
    bool AddFile(const std::string& filePath, uintptr_t fileData, DWORD fileSize);
    bool RemoveFile(const std::string& filePath);
    bool RenameFile(const std::string& oldFilePath, const std::string& newFilePath);
    std::shared_ptr<std::vector<std::string>> ListFiles(const std::string& fileSearchMask);
    bool HasFile(const std::string& fileSearchMask);
    const std::string* HashToString(uint64_t hash) const;
    std::vector<uint32_t> GetGameVersions();
    void PushGameVersion(uint32_t newGameVersion);

  protected:
    std::shared_ptr<std::vector<SFILE_FIND_DATA>> FindFiles(const std::string& fileSearchMask);
    bool Load(bool enableWriting, bool generateCrcMap);
    bool Unload();

  private:
    std::string mMainPath;
    std::string mPatchesPath;
    std::vector<std::string> mOtrStormArchives;
    std::unordered_set<uint32_t> mValidHashes;
    std::map<std::string, HANDLE> mMpqHandles;
    std::vector<std::string> mAddedFiles;
    std::vector<uint32_t> mGameVersions;
    std::unordered_map<uint64_t, std::string> mHashes;
    HANDLE mMainMpq;
    std::mutex mMutex;

    bool LoadMainMPQ(bool enableWriting, bool generateCrcMap);
    bool LoadPatchMPQs();
    bool LoadPatchMPQ(const std::string& otrPath, bool validateVersion = false);
    void GenerateCrcMap();
    bool ProcessOtrVersion(HANDLE mpqHandle = nullptr);
    std::shared_ptr<StormFile> LoadFileFromHandle(const std::string& filePath, bool includeParent = true,
                                             HANDLE mpqHandle = nullptr);
};
