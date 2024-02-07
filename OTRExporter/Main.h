#pragma once

#include <libultraship/bridge.h>
#include "StormArchive.h"

extern std::shared_ptr<StormArchive> otrArchive;
extern std::map<std::string, std::vector<char>> files;

void AddFile(std::string fName, std::vector<char> data);