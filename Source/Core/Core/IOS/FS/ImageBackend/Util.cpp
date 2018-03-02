// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/FS/ImageBackend/Util.h"

#include "Common/CommonTypes.h"
#include "Core/IOS/FS/ImageBackend/SFFS.h"

namespace IOS::HLE::FS
{
bool HasPermission(const FstEntry& fst_entry, Uid uid, Gid gid, Mode requested_mode)
{
  if (uid == 0)
    return true;

  Mode file_mode = Mode::None;
  if (fst_entry.uid == uid)
    file_mode = fst_entry.GetOwnerMode();
  else if (fst_entry.gid == gid)
    file_mode = fst_entry.GetGroupMode();
  else
    file_mode = fst_entry.GetOtherMode();
  return (u8(requested_mode) & u8(file_mode)) == u8(requested_mode);
}

bool IsValidNonRootPath(const std::string& path)
{
  return path.length() > 1 && path.length() <= 64 && path[0] == '/' && *path.rbegin() != '/';
}

SplitPathResult SplitPath(const std::string& path)
{
  const auto last_separator = path.find_last_of('/');
  return {path.substr(0, last_separator + 1), path.substr(last_separator + 1)};
}

}  // namespace IOS::HLE::FS
