// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "Core/IOS/FS/FileSystem.h"

namespace IOS::HLE::FS
{
struct FstEntry;

bool HasPermission(const FstEntry& fst_entry, Uid uid, Gid gid, Mode requested_mode);

bool IsValidNonRootPath(const std::string& path);

struct SplitPathResult
{
  std::string parent;
  std::string file_name;
};
/// Split a path into a parent path and the file name. Takes a *valid non-root* path.
///
/// Example: /shared2/sys/SYSCONF => {/shared2/sys, SYSCONF}
SplitPathResult SplitPath(const std::string& path);

}  // namespace IOS::HLE::FS
