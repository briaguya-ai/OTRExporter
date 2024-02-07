#include "StormArchive.h"
#include "Resource.h"
#include "StormFile.h"
#include "resource/ResourceManager.h"
#include "Utils/StringHelper.h"
#include <StrHash64.h>
#include <filesystem>
#include "utils/binarytools/MemoryStream.h"
#include "utils/binarytools/FileHelper.h"

#ifdef __SWITCH__
#include "port/switch/SwitchImpl.h"
#endif

StormArchive::StormArchive(const std::string& mainPath, bool enableWriting)
    : StormArchive(mainPath, "", std::unordered_set<uint32_t>(), enableWriting) {
    mMainMpq = nullptr;
}

StormArchive::StormArchive(const std::string& mainPath, const std::string& patchesPath,
                 const std::unordered_set<uint32_t>& validHashes, bool enableWriting, bool generateCrcMap)
    : mMainPath(mainPath), mPatchesPath(patchesPath), mOtrStormArchives({}), mValidHashes(validHashes) {
    mMainMpq = nullptr;
    Load(enableWriting, generateCrcMap);
}

StormArchive::StormArchive(const std::vector<std::string>& fileList, const std::unordered_set<uint32_t>& validHashes,
                 bool enableWriting, bool generateCrcMap)
    : mOtrStormArchives(fileList), mValidHashes(validHashes) {
    mMainMpq = nullptr;
    Load(enableWriting, generateCrcMap);
}

StormArchive::~StormArchive() {
    Unload();
}

bool StormArchive::IsMainMPQValid() {
    return mMainMpq != nullptr;
}

std::shared_ptr<StormArchive> StormArchive::CreateArchive(const std::string& archivePath, size_t fileCapacity) {
    auto archive = std::make_shared<StormArchive>(archivePath, true);

    TCHAR* fileName = new TCHAR[archivePath.size() + 1];
    fileName[archivePath.size()] = 0;
    std::copy(archivePath.begin(), archivePath.end(), fileName);

    bool success;
    {
        const std::lock_guard<std::mutex> lock(archive->mMutex);
        success = SFileCreateArchive(fileName, MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES | MPQ_CREATE_ARCHIVE_V2,
                                     fileCapacity, &archive->mMainMpq);
    }
    int32_t error = GetLastError();

    delete[] fileName;

    if (success) {
        archive->mMpqHandles[archivePath] = archive->mMainMpq;
        return archive;
    } else {
        // printf(error);
        printf("We tried to create an StormArchive, but it has fallen and cannot get up.\n");
        return nullptr;
    }
}

std::shared_ptr<File> StormArchive::LoadFileFromHandle(const std::string& filePath, bool includeParent, HANDLE mpqHandle) {
    HANDLE fileHandle = NULL;

    std::shared_ptr<File> fileToLoad = std::make_shared<File>();
    fileToLoad->Path = filePath;

    if (mpqHandle == nullptr) {
        mpqHandle = mMainMpq;
    }

#if _DEBUG
    if (FileHelper::Exists("TestData/" + filePath)) {
        auto byteData = FileHelper::ReadAllBytes("TestData/" + filePath);
        fileToLoad->Buffer.resize(byteData.size() + 1);
        memcpy(fileToLoad->Buffer.data(), byteData.data(), byteData.size() + 1);

        // Throw in a null terminator at the end incase we're loading a text file...
        fileToLoad->Buffer[byteData.size()] = '\0';

        fileToLoad->Parent = includeParent ? shared_from_this() : nullptr;
        fileToLoad->IsLoaded = true;
    } else {
#endif
        bool attempt;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            attempt = SFileOpenFileEx(mpqHandle, filePath.c_str(), 0, &fileHandle);
        }

        if (!attempt) {
            SPDLOG_TRACE("({}) Failed to open file {} from mpq StormArchive  {}.", GetLastError(), filePath, mMainPath);
            return nullptr;
        }

        DWORD fileSize;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            fileSize = SFileGetFileSize(fileHandle, 0);
        }
        fileToLoad->Buffer.resize(fileSize);
        DWORD countBytes;

        bool readFileSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            readFileSuccess = SFileReadFile(fileHandle, fileToLoad->Buffer.data(), fileSize, &countBytes, NULL);
        }
        if (!readFileSuccess) {
            SPDLOG_ERROR("({}) Failed to read file {} from mpq StormArchive {}", GetLastError(), filePath, mMainPath);
            bool closeFileSuccess;
            {
                const std::lock_guard<std::mutex> lock(mMutex);
                closeFileSuccess = SFileCloseFile(fileHandle);
            }
            if (!closeFileSuccess) {
                SPDLOG_ERROR("({}) Failed to close file {} from mpq after read failure in StormArchive {}", GetLastError(),
                             filePath, mMainPath);
            }
            return nullptr;
        }

        bool closeFileSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            closeFileSuccess = SFileCloseFile(fileHandle);
        }
        if (!closeFileSuccess) {
            SPDLOG_ERROR("({}) Failed to close file {} from mpq StormArchive {}", GetLastError(), filePath, mMainPath);
        }

        fileToLoad->Parent = includeParent ? shared_from_this() : nullptr;
        fileToLoad->IsLoaded = true;
#if _DEBUG
    }
#endif

    return fileToLoad;
}

std::shared_ptr<File> StormArchive::LoadFile(const std::string& filePath, bool includeParent) {
    return LoadFileFromHandle(filePath, includeParent, nullptr);
}

bool StormArchive::AddFile(const std::string& filePath, uintptr_t fileData, DWORD fileSize) {
    HANDLE hFile;
#ifdef _WIN32
    SYSTEMTIME sysTime;
    GetSystemTime(&sysTime);
    FILETIME t;
    SystemTimeToFileTime(&sysTime, &t);
    ULONGLONG theTime = static_cast<uint64_t>(t.dwHighDateTime) << (sizeof(t.dwHighDateTime) * 8) | t.dwLowDateTime;
#else
    time_t theTime;
    time(&theTime);
#endif

    std::string updatedPath = filePath;

    StringHelper::ReplaceOriginal(updatedPath, "\\", "/");

    bool createFileSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        createFileSuccess =
            SFileCreateFile(mMainMpq, updatedPath.c_str(), theTime, fileSize, 0, MPQ_FILE_COMPRESS, &hFile);
    }
    if (!createFileSuccess) {
        SPDLOG_ERROR("({}) Failed to create file of {} bytes {} in StormArchive {}", GetLastError(), fileSize, updatedPath,
                     mMainPath);
        return false;
    }

    bool writeFileSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        writeFileSuccess = SFileWriteFile(hFile, (void*)fileData, fileSize, MPQ_COMPRESSION_ZLIB);
    }
    if (!writeFileSuccess) {
        SPDLOG_ERROR("({}) Failed to write {} bytes to {} in StormArchive {}", GetLastError(), fileSize, updatedPath,
                     mMainPath);
        bool closeFileSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            closeFileSuccess = SFileCloseFile(hFile);
        }
        if (!closeFileSuccess) {
            SPDLOG_ERROR("({}) Failed to close file {} after write failure in StormArchive {}", GetLastError(), updatedPath,
                         mMainPath);
        }
        return false;
    }

    bool finishFileSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        finishFileSuccess = SFileFinishFile(hFile);
    }
    if (!finishFileSuccess) {
        SPDLOG_ERROR("({}) Failed to finish file {} in StormArchive {}", GetLastError(), updatedPath, mMainPath);
        bool closeFileSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            closeFileSuccess = SFileCloseFile(hFile);
        }
        if (!closeFileSuccess) {
            SPDLOG_ERROR("({}) Failed to close file {} after finish failure in StormArchive {}", GetLastError(), updatedPath,
                         mMainPath);
        }
        return false;
    }
    // SFileFinishFile already frees the handle, so no need to close it again.

    mAddedFiles.push_back(updatedPath);
    mHashes[CRC64(updatedPath.c_str())] = updatedPath;

    return true;
}

bool StormArchive::RemoveFile(const std::string& filePath) {
    // TODO: Notify the resource manager and child Files

    bool removeFileSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        removeFileSuccess = SFileRemoveFile(mMainMpq, filePath.c_str(), 0);
    }
    if (!removeFileSuccess) {
        SPDLOG_ERROR("({}) Failed to remove file {} in StormArchive {}", GetLastError(), filePath, mMainPath);
        return false;
    }

    return true;
}

bool StormArchive::RenameFile(const std::string& oldFilePath, const std::string& newFilePath) {
    // TODO: Notify the resource manager and child Files

    bool renameFileSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        renameFileSuccess = SFileRenameFile(mMainMpq, oldFilePath.c_str(), newFilePath.c_str());
    }
    if (!renameFileSuccess) {
        SPDLOG_ERROR("({}) Failed to rename file {} to {} in StormArchive {}", GetLastError(), oldFilePath, newFilePath,
                     mMainPath);
        return false;
    }

    return true;
}

std::shared_ptr<std::vector<SFILE_FIND_DATA>> StormArchive::FindFiles(const std::string& fileSearchMask) {
    auto fileList = std::make_shared<std::vector<SFILE_FIND_DATA>>();
    SFILE_FIND_DATA findContext;
    HANDLE hFind;

    {
        const std::lock_guard<std::mutex> lock(mMutex);
        hFind = SFileFindFirstFile(mMainMpq, fileSearchMask.c_str(), &findContext, nullptr);
    }
    if (hFind != nullptr) {
        fileList->push_back(findContext);

        bool fileFound;
        do {
            {
                const std::lock_guard<std::mutex> lock(mMutex);
                fileFound = SFileFindNextFile(hFind, &findContext);
            }
            if (fileFound) {
                fileList->push_back(findContext);
            } else if (!fileFound && GetLastError() != ERROR_NO_MORE_FILES) {
                SPDLOG_ERROR("({}), Failed to search with mask {} in StormArchive {}", GetLastError(), fileSearchMask,
                             mMainPath);
                if (!SListFileFindClose(hFind)) {
                    SPDLOG_ERROR("({}) Failed to close file search {} after failure in StormArchive {}", GetLastError(),
                                 fileSearchMask, mMainPath);
                }
                return fileList;
            }
        } while (fileFound);
    } else if (GetLastError() != ERROR_NO_MORE_FILES) {
        SPDLOG_ERROR("({}), Failed to search with mask {} in StormArchive {}", GetLastError(), fileSearchMask, mMainPath);
        return fileList;
    }

    if (hFind != nullptr) {
        bool fileFindCloseSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            fileFindCloseSuccess = SFileFindClose(hFind);
        }
        if (!fileFindCloseSuccess) {
            SPDLOG_ERROR("({}) Failed to close file search {} in StormArchive {}", GetLastError(), fileSearchMask,
                         mMainPath);
        }

        return fileList;
    }

    return fileList;
}

std::shared_ptr<std::vector<std::string>> StormArchive::ListFiles(const std::string& fileSearchMask) {
    auto result = std::make_shared<std::vector<std::string>>();
    auto fileList = FindFiles(fileSearchMask);

    for (size_t i = 0; i < fileList->size(); i++) {
        result->push_back(fileList->operator[](i).cFileName);
    }

    return result;
}

bool StormArchive::HasFile(const std::string& fileSearchMask) {
    auto list = FindFiles(fileSearchMask);
    return list->size() > 0;
}

const std::string* StormArchive::HashToString(uint64_t hash) const {
    auto it = mHashes.find(hash);
    return it != mHashes.end() ? &it->second : nullptr;
}

bool StormArchive::Load(bool enableWriting, bool generateCrcMap) {
    return LoadMainMPQ(enableWriting, generateCrcMap) && LoadPatchMPQs();
}

bool StormArchive::Unload() {
    bool success = true;
    for (const auto& mpqHandle : mMpqHandles) {
        bool closeStormArchiveSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            closeStormArchiveSuccess = SFileCloseStormArchive(mpqHandle.second);
        }
        if (!closeStormArchiveSuccess) {
            SPDLOG_ERROR("({}) Failed to close mpq {}", GetLastError(), mpqHandle.first);
            success = false;
        }
    }

    mMainMpq = nullptr;

    return success;
}

bool StormArchive::LoadPatchMPQs() {
    // OTRTODO: We also want to periodically scan the patch directories for new MPQs. When new MPQs are found we will
    // load the contents to fileCache and then copy over to gameResourceAddresses
    if (mPatchesPath.length() > 0) {
        if (std::filesystem::is_directory(mPatchesPath)) {
            for (const auto& p : std::filesystem::recursive_directory_iterator(mPatchesPath)) {
                if (StringHelper::IEquals(p.path().extension().string(), ".otr") ||
                    StringHelper::IEquals(p.path().extension().string(), ".mpq")) {
                    SPDLOG_ERROR("Reading {} mpq patch", p.path().string());
                    if (!LoadPatchMPQ(p.path().string())) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

void StormArchive::GenerateCrcMap() {
    auto listFile = LoadFile("(listfile)", false);

    // Use std::string_view to avoid unnecessary string copies
    std::vector<std::string_view> lines =
        StringHelper::Split(std::string_view(listFile->Buffer.data(), listFile->Buffer.size()), "\n");

    for (size_t i = 0; i < lines.size(); i++) {
        // Use std::string_view to avoid unnecessary string copies
        std::string_view line = lines[i].substr(0, lines[i].length() - 1); // Trim \r
        std::string lineStr = std::string(line);

        // Not NULL terminated str
        uint64_t hash = ~crc64(line.data(), line.length());
        mHashes.emplace(hash, std::move(lineStr));
    }
}

bool StormArchive::ProcessOtrVersion(HANDLE mpqHandle) {
    auto t = LoadFileFromHandle("version", false, mpqHandle);
    if (t != nullptr && t->IsLoaded) {
        auto stream = std::make_shared<MemoryStream>(t->Buffer.data(), t->Buffer.size());
        auto reader = std::make_shared<BinaryReader>(stream);
        LUS::Endianness endianness = (LUS::Endianness)reader->ReadUByte();
        reader->SetEndianness(endianness);
        uint32_t version = reader->ReadUInt32();
        // Game version found so track it if it matches or there is nothing to match against
        if (mValidHashes.empty() || mValidHashes.contains(version)) {
            PushGameVersion(version);
            return true;
        }
    }

    // Allow the otr through if there are no valid hashes anyways
    return mValidHashes.empty();
}

bool StormArchive::LoadMainMPQ(bool enableWriting, bool generateCrcMap) {
    HANDLE mpqHandle = NULL;
    if (mOtrStormArchives.empty()) {
        if (mMainPath.length() > 0) {
            if (std::filesystem::is_directory(mMainPath)) {
                for (const auto& p : std::filesystem::recursive_directory_iterator(mMainPath)) {
                    if (StringHelper::IEquals(p.path().extension().string(), ".otr")) {
                        SPDLOG_ERROR("Reading {} mpq", p.path().string());
                        mOtrStormArchives.push_back(p.path().string());
                    }
                }
            } else if (std::filesystem::is_regular_file(mMainPath)) {
                mOtrStormArchives.push_back(mMainPath);
            } else {
                SPDLOG_ERROR("The directory {} does not exist", mMainPath);
                return false;
            }
        } else {
            SPDLOG_ERROR("No OTR file list or Main Path provided.");
            return false;
        }
        if (mOtrStormArchives.empty()) {
            SPDLOG_ERROR("No OTR files present in {}", mMainPath);
            return false;
        }
    }
    bool baseLoaded = false;
    size_t i = 0;
    while (!baseLoaded && i < mOtrStormArchives.size()) {
#if defined(__SWITCH__) || defined(__WIIU__)
        std::string fullPath = mOtrStormArchives[i];
#else
        std::string fullPath = std::filesystem::absolute(mOtrStormArchives[i]).string();
#endif
        bool openStormArchiveSuccess;
        {
            const std::lock_guard<std::mutex> lock(mMutex);
            openStormArchiveSuccess =
                SFileOpenStormArchive(fullPath.c_str(), 0, enableWriting ? 0 : MPQ_OPEN_READ_ONLY, &mpqHandle);
        }
        if (openStormArchiveSuccess) {
            SPDLOG_INFO("Opened mpq file {}.", fullPath);
            mMainMpq = mpqHandle;
            mMainPath = fullPath;
            if (!ProcessOtrVersion(mMainMpq)) {
                SPDLOG_WARN("Attempted to load invalid OTR file {}", mOtrStormArchives[i]);
                {
                    const std::lock_guard<std::mutex> lock(mMutex);
                    SFileCloseStormArchive(mpqHandle);
                }
                mMainMpq = nullptr;
            } else {
                mMpqHandles[fullPath] = mpqHandle;
                if (generateCrcMap) {
                    GenerateCrcMap();
                }
                baseLoaded = true;
            }
        }
        i++;
    }
    // If we exited the above loop without setting baseLoaded to true, then we've
    // attemtped to load all the OTRs available to us.
    if (!baseLoaded) {
        SPDLOG_ERROR("No valid OTR file was provided.");
        return false;
    }
    for (size_t j = i; j < mOtrStormArchives.size(); j++) {
#if defined(__SWITCH__) || defined(__WIIU__)
        std::string fullPath = mOtrStormArchives[j];
#else
        std::string fullPath = std::filesystem::absolute(mOtrStormArchives[j]).string();
#endif
        if (LoadPatchMPQ(fullPath, true)) {
            SPDLOG_INFO("({}) Patched in mpq file.", fullPath);
        }
        if (generateCrcMap) {
            GenerateCrcMap();
        }
    }

    return true;
}

bool StormArchive::LoadPatchMPQ(const std::string& otrPath, bool validateVersion) {
    HANDLE patchHandle = NULL;
#if defined(__SWITCH__) || defined(__WIIU__)
    std::string fullPath = otrPath;
#else
    std::string fullPath = std::filesystem::absolute(otrPath).string();
#endif
    if (mMpqHandles.contains(fullPath)) {
        return true;
    }
    bool openStormArchiveSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        openStormArchiveSuccess = SFileOpenStormArchive(fullPath.c_str(), 0, MPQ_OPEN_READ_ONLY, &patchHandle);
    }
    if (!openStormArchiveSuccess) {
        SPDLOG_ERROR("({}) Failed to open patch mpq file {} while applying to {}.", GetLastError(), otrPath, mMainPath);
        return false;
    } else {
        // We don't always want to validate the "version" file, only when we're loading standalone OTRs as patches
        // i.e. Ocarina of Time along with Master Quest.
        if (validateVersion) {
            if (!ProcessOtrVersion(patchHandle)) {
                SPDLOG_INFO("({}) Missing version file. Attempting to apply patch anyway.", otrPath);
            }
        }
    }
    bool openPatchStormArchiveSuccess;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        openPatchStormArchiveSuccess = SFileOpenPatchStormArchive(mMainMpq, fullPath.c_str(), "", 0);
    }
    if (!openPatchStormArchiveSuccess) {
        SPDLOG_ERROR("({}) Failed to apply patch mpq file {} to main mpq {}.", GetLastError(), otrPath, mMainPath);
        return false;
    }

    mMpqHandles[fullPath] = patchHandle;

    return true;
}

std::vector<uint32_t> StormArchive::GetGameVersions() {
    return mGameVersions;
}

void StormArchive::PushGameVersion(uint32_t newGameVersion) {
    mGameVersions.push_back(newGameVersion);
}
