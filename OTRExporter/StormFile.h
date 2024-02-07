#pragma once

#include <string>
#include <vector>
#include <memory>

class StormArchive;

struct StormFile {
    std::shared_ptr<StormArchive> Parent;
    std::string Path;
    std::vector<char> Buffer;
    bool IsLoaded = false;
};
