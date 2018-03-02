// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <tuple>

#include "Core/IOS/FS/ImageBackend/SFFS.h"

namespace IOS::HLE::FS
{
std::string FstEntry::GetName() const
{
  return {name.data(), strnlen(name.data(), name.size())};
}

void FstEntry::SetName(const std::string& new_name)
{
  name.fill(0);
  std::copy_n(new_name.data(), std::min<size_t>(new_name.size(), 12), name.begin());
}

bool FstEntry::IsFile() const
{
  return (mode & 3) == 1;
}

bool FstEntry::IsDirectory() const
{
  return (mode & 3) == 2;
}

Mode FstEntry::GetOwnerMode() const
{
  return static_cast<Mode>((mode >> 0x6) & 3);
}

Mode FstEntry::GetGroupMode() const
{
  return static_cast<Mode>(((mode & 0x30) >> 4) & 3);
}

Mode FstEntry::GetOtherMode() const
{
  return static_cast<Mode>(((mode & 0xc) >> 2) & 3);
}

void FstEntry::SetAccessMode(Mode owner, Mode group, Mode other)
{
  mode = (mode & 3) | (u8(owner) << 6) | 16 * u8(group) | 4 * u8(other);
}

bool operator==(const FstEntry& lhs, const FstEntry& rhs)
{
  auto fields = [](const FstEntry& e) {
    return std::tie(e.name, e.mode, e.attr, e.sub, e.sib, e.size, e.uid, e.gid, e.x3);
  };
  return fields(lhs) == fields(rhs);
}

bool operator!=(const FstEntry& lhs, const FstEntry& rhs)
{
  return !operator==(lhs, rhs);
}

bool operator==(const Superblock& lhs, const Superblock& rhs)
{
  auto fields = [](const auto& s) { return std::tie(s.magic, s.version, s.unknown, s.fat, s.fst); };
  return fields(lhs) == fields(rhs);
}

bool operator!=(const Superblock& lhs, const Superblock& rhs)
{
  return !operator==(lhs, rhs);
}

}  // namespace IOS::HLE::FS
