// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/IOS/FS/ImageBackend/FS.h"
#include "Core/IOS/FS/ImageBackend/Util.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"

namespace IOS::HLE::FS
{
NandFileSystem::NandFileSystem(const std::string& nand_path, IOSC& iosc)
    : m_iosc{iosc}, m_nand{nand_path, "rb+"}, m_nand_path{nand_path},
      m_block_mac_generator{iosc.GetNandMacGenerator()}
{
  auto* superblock = GetSuperblock();
  if (!superblock)
    return;

  for (auto& cluster : superblock->fat)
    if (cluster == 0xffff)
      cluster = CLUSTER_UNUSED;
}

NandFileSystem::~NandFileSystem() = default;

static void GetUsedClusters(const Superblock& superblock, u16 directory, std::vector<u16>* clusters)
{
  const u16 sub = superblock.fst[directory].sub;
  // Traverse the directory
  for (u16 child = sub; child < superblock.fst.size(); child = superblock.fst[child].sib)
  {
    if (superblock.fst[child].IsDirectory())
    {
      GetUsedClusters(superblock, child, clusters);
      continue;
    }

    u16 c = superblock.fst[child].sub;
    while (c < superblock.fat.size())
    {
      clusters->emplace_back(c);
      c = superblock.fat[c];
    }
  }
}

void NandFileSystem::DoState(PointerWrap& p)
{
  p.Do(m_handles);
  p.Do(m_cache_fd);
  p.Do(m_cache_chain_index);
  p.Do(m_cache_data);
  p.Do(m_cache_for_write);

  p.Do(m_nand_path);
  m_nand = File::IOFile{m_nand_path, "rb+"};
  ASSERT(m_nand.Seek(0, SEEK_SET));

  constexpr std::array<char, 4> NO_SUPERBLOCK_MAGIC{{'X', 'X', 'X', 'X'}};
  Superblock superblock = m_superblock ? *m_superblock : Superblock{NO_SUPERBLOCK_MAGIC};
  const Superblock old_superblock = superblock;
  p.Do(m_superblock_index);
  p.Do(superblock);
  m_superblock =
      superblock.magic != NO_SUPERBLOCK_MAGIC ? std::make_unique<Superblock>(superblock) : nullptr;

  // Optimisation: only save clusters that are actually used
  std::vector<u16> used_clusters;
  if (m_superblock && p.GetMode() != PointerWrap::MODE_READ)
    GetUsedClusters(*m_superblock, 0, &used_clusters);
  std::sort(used_clusters.begin(), used_clusters.end());
  p.Do(used_clusters);

  constexpr size_t cluster_size = PAGES_PER_CLUSTER * (DATA_BYTES_PER_PAGE + SPARE_BYTES_PER_PAGE);
  if (p.GetMode() == PointerWrap::MODE_WRITE)
  {
    auto data = std::make_unique<u8[]>(cluster_size);
    for (const u16 cluster : used_clusters)
    {
      ASSERT(m_nand.Seek(Offset(cluster), SEEK_SET));
      ASSERT(m_nand.ReadBytes(data.get(), cluster_size));
      p.DoArray(data.get(), cluster_size);
    }
  }
  else if (p.GetMode() == PointerWrap::MODE_READ && old_superblock != superblock)
  {
    // Optimisation: only reload the NAND data from the savestate if it has changed.
    auto data = std::make_unique<u8[]>(cluster_size);
    for (const u16 cluster : used_clusters)
    {
      p.DoArray(data.get(), cluster_size);
      ASSERT(m_nand.Seek(Offset(cluster), SEEK_SET));
      ASSERT(m_nand.WriteBytes(data.get(), cluster_size));
    }
  }
  else
  {
    *p.ptr += cluster_size * used_clusters.size();
  }

  ASSERT(m_nand.Seek(Offset(SuperblockCluster(0)), SEEK_SET));
  std::vector<u8> data(cluster_size * CLUSTERS_PER_SUPERBLOCK * NUMBER_OF_SUPERBLOCKS);
  if (p.GetMode() == PointerWrap::MODE_WRITE)
    ASSERT(m_nand.ReadArray(data.data(), data.size()));
  p.Do(data);
  if (p.GetMode() == PointerWrap::MODE_READ)
    ASSERT(m_nand.WriteArray(data.data(), data.size()));
}

ResultCode NandFileSystem::Format(Uid uid)
{
  if (uid != 0)
    return ResultCode::AccessDenied;

  if (!GetSuperblock())
    m_superblock = std::make_unique<Superblock>();

  m_superblock->magic = {{'S', 'F', 'F', 'S'}};

  for (size_t i = 0; i < m_superblock->fat.size(); ++i)
  {
    // Mark the boot1, boot2 and FS metadata regions as reserved
    if (i < 64 || i >= SUPERBLOCK_START_CLUSTER)
      m_superblock->fat[i] = CLUSTER_RESERVED;
    else
      m_superblock->fat[i] = CLUSTER_UNUSED;
  }

  // Initialise the FST
  m_superblock->fst.fill(FstEntry{});
  FstEntry* root = &m_superblock->fst[0];
  root->SetName("/");
  root->mode = 0x16;
  root->sub = 0xffff;
  root->sib = 0xffff;

  for (Handle& handle : m_handles)
    handle.opened = false;

  return FlushSuperblock();
}

ResultCode NandFileSystem::CreateFileOrDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path, FileAttribute attribute,
                                                 Modes modes, bool is_file)
{
  if (!IsValidNonRootPath(path) ||
      std::any_of(path.begin(), path.end(), [](char c) { return c - ' ' > 0x5e; }))
  {
    return ResultCode::Invalid;
  }

  if (!is_file && std::count(path.begin(), path.end(), '/') > 8)
    return ResultCode::TooManyPathComponents;

  auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const auto split_path = SplitPath(path);
  const Result<u16> parent_idx = GetFstIndex(*superblock, split_path.parent);
  if (!parent_idx)
    return ResultCode::NotFound;

  FstEntry* parent = &superblock->fst[*parent_idx];
  if (!HasPermission(*parent, caller_uid, caller_gid, Mode::Write))
    return ResultCode::AccessDenied;

  if (GetFstIndex(*superblock, *parent_idx, split_path.file_name))
    return ResultCode::AlreadyExists;

  const Result<u16> child_idx = GetUnusedFstIndex(*superblock);
  if (!child_idx)
    return ResultCode::FstFull;

  FstEntry* child = &superblock->fst[*child_idx];
  child->SetName(split_path.file_name);
  child->mode = is_file ? 1 : 2;
  child->SetAccessMode(modes.owner, modes.group, modes.other);
  child->uid = caller_uid;
  child->gid = caller_gid;
  child->size = 0;
  child->x3 = 0;
  child->attr = attribute;
  child->sub = is_file ? CLUSTER_LAST_IN_CHAIN : 0xffff;
  child->sib = parent->sub;
  parent->sub = *child_idx;
  return FlushSuperblock();
}

ResultCode NandFileSystem::CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                                      FileAttribute attribute, Modes modes)
{
  return CreateFileOrDirectory(caller_uid, caller_gid, path, attribute, modes, true);
}

ResultCode NandFileSystem::CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                                           FileAttribute attribute, Modes modes)
{
  return CreateFileOrDirectory(caller_uid, caller_gid, path, attribute, modes, false);
}

/// Delete a file.
/// A valid file FST index must be passed.
static void DeleteFile(Superblock* superblock, u16 file)
{
  // Free all clusters that were used by the file.
  for (u16 i = superblock->fst[file].sub; i < superblock->fat.size();)
  {
    const u16 next = superblock->fat[i];
    superblock->fat[i] = CLUSTER_UNUSED;
    i = next;
  }

  // Remove its entry from the FST.
  superblock->fst[file].mode = 0;
}

/// Recursively delete all files in a directory (without flushing the superblock).
/// A valid directory FST index must be passed and contained files must all be closed.
static void DeleteDirectoryContents(Superblock* superblock, u16 directory)
{
  const u16 sub = superblock->fst[directory].sub;
  // Traverse the directory
  for (u16 child = sub; child < superblock->fst.size(); child = superblock->fst[child].sib)
  {
    if (superblock->fst[child].IsDirectory())
    {
      DeleteDirectoryContents(superblock, child);
    }
    else
    {
      DeleteFile(superblock, child);
    }
  }
}

/// Remove a FST entry (file or directory) from a chain.
/// A valid FST entry index and its parent index must be passed.
static ResultCode RemoveFstEntryFromChain(Superblock* superblock, u16 parent, u16 child)
{
  // First situation: the parent's sub points to the entry we want to remove.
  //
  // +--------+  sub  +-------+  sib  +------+  sib
  // | parent |------>| child |------>| next |------> ...
  // +--------+       +-------+       +------+
  //
  // After removing the first child entry, the tree should be like this:
  //
  // +--------+  sub                  +------+  sib
  // | parent |---------------------->| next |------> ...
  // +--------+                       +------+
  //
  if (superblock->fst[parent].sub == child)
  {
    superblock->fst[parent].sub = superblock->fst[child].sib;
    superblock->fst[child].mode = 0;
    return ResultCode::Success;
  }

  // Second situation: the entry to remove is between two sibling nodes.
  //
  // +--------+  sub         sib  +----------+  sib  +-------+  sib  +------+
  // | parent |------> ... ------>| previous |------>| child |------>| next |-----> ...
  // +--------+                   +----------+       +-------+       +------+
  //
  // We should end up with this:
  //
  // +--------+  sub         sib  +----------+  sib                  +------+
  // | parent |------> ... ------>| previous |---------------------->| next |-----> ...
  // +--------+                   +----------+                       +------+
  //
  u16 previous = superblock->fst[parent].sub;
  u16 index = superblock->fst[previous].sib;
  while (index < superblock->fst.size())
  {
    if (index == child)
    {
      superblock->fst[previous].sib = superblock->fst[child].sib;
      superblock->fst[child].mode = 0;
      return ResultCode::Success;
    }
    previous = index;
    index = superblock->fst[index].sib;
  }

  return ResultCode::NotFound;
}

ResultCode NandFileSystem::Delete(Uid caller_uid, Gid caller_gid, const std::string& path)
{
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;

  auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const auto split_path = SplitPath(path);
  const Result<u16> parent = GetFstIndex(*superblock, split_path.parent);
  if (!parent)
    return ResultCode::NotFound;

  if (!HasPermission(superblock->fst[*parent], caller_uid, caller_gid, Mode::Write))
    return ResultCode::AccessDenied;

  const Result<u16> index = GetFstIndex(*superblock, *parent, split_path.file_name);
  if (!index)
    return ResultCode::NotFound;

  const FstEntry& entry = superblock->fst[*index];
  if (entry.IsDirectory() && !IsDirectoryInUse(*superblock, *index))
    DeleteDirectoryContents(superblock, *index);
  else if (entry.IsFile() && !IsFileOpened(*index))
    DeleteFile(superblock, *index);
  else
    return ResultCode::InUse;

  const ResultCode result = RemoveFstEntryFromChain(superblock, *parent, *index);
  if (result != ResultCode::Success)
    return result;

  return FlushSuperblock();
}

ResultCode NandFileSystem::Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                                  const std::string& new_path)
{
  if (!IsValidNonRootPath(old_path) || !IsValidNonRootPath(new_path))
    return ResultCode::Invalid;

  auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const auto split_old_path = SplitPath(old_path);
  const auto split_new_path = SplitPath(new_path);

  const Result<u16> old_parent = GetFstIndex(*superblock, split_old_path.parent);
  const Result<u16> new_parent = GetFstIndex(*superblock, split_new_path.parent);
  if (!old_parent || !new_parent)
    return ResultCode::NotFound;

  if (!HasPermission(superblock->fst[*old_parent], caller_uid, caller_gid, Mode::Write) ||
      !HasPermission(superblock->fst[*new_parent], caller_uid, caller_gid, Mode::Write))
  {
    return ResultCode::AccessDenied;
  }

  const Result<u16> index = GetFstIndex(*superblock, *old_parent, split_old_path.file_name);
  if (!index)
    return ResultCode::NotFound;

  FstEntry* entry = &superblock->fst[*index];
  if (entry->IsFile() &&
      split_old_path.file_name.substr(0, 12) != split_new_path.file_name.substr(0, 12))
  {
    return ResultCode::Invalid;
  }

  if ((entry->IsDirectory() && IsDirectoryInUse(*superblock, *index)) ||
      (entry->IsFile() && IsFileOpened(*index)))
  {
    return ResultCode::InUse;
  }

  // If there is already something of the same type at the new path, delete it.
  const Result<u16> new_index = GetFstIndex(*superblock, *new_parent, split_new_path.file_name);
  if (new_index)
  {
    if ((superblock->fst[*new_index].mode & 3) != (entry->mode & 3) || *new_index == *index)
      return ResultCode::Invalid;

    if (superblock->fst[*new_index].IsDirectory() && !IsDirectoryInUse(*superblock, *new_index))
      DeleteDirectoryContents(superblock, *new_index);
    else if (superblock->fst[*new_index].IsFile() && !IsFileOpened(*new_index))
      DeleteFile(superblock, *new_index);
    else
      return ResultCode::InUse;

    const auto remove_result = RemoveFstEntryFromChain(superblock, *new_parent, *new_index);
    if (remove_result != ResultCode::Success)
      return remove_result;
  }

  const u8 saved_mode = entry->mode;
  const auto remove_result = RemoveFstEntryFromChain(superblock, *old_parent, *index);
  if (remove_result != ResultCode::Success)
    return remove_result;

  entry->mode = saved_mode;
  entry->SetName(split_new_path.file_name);
  entry->sib = superblock->fst[*new_parent].sub;
  superblock->fst[*new_parent].sub = *index;

  return FlushSuperblock();
}

Result<std::vector<std::string>> NandFileSystem::ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                               const std::string& path)
{
  if (path.empty() || path.length() > 64 || path[0] != '/')
    return ResultCode::Invalid;

  const auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const Result<u16> index = GetFstIndex(*superblock, path);
  if (!index)
    return ResultCode::NotFound;

  if (!HasPermission(superblock->fst[*index], caller_uid, caller_gid, Mode::Read))
    return ResultCode::AccessDenied;

  if (!superblock->fst[*index].IsDirectory())
    return ResultCode::Invalid;

  std::vector<std::string> children;
  for (u16 i = superblock->fst[*index].sub; i != 0xffff; i = superblock->fst[i].sib)
  {
    children.emplace_back(superblock->fst[i].GetName());
  }
  return children;
}

Result<Metadata> NandFileSystem::GetMetadata(Uid caller_uid, Gid caller_gid,
                                             const std::string& path)
{
  if (path.empty())
    return ResultCode::Invalid;

  const auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  u16 index;
  if (path == "/")
  {
    index = 0;
  }
  else if (IsValidNonRootPath(path))
  {
    const auto split_path = SplitPath(path);

    const Result<u16> parent = GetFstIndex(*superblock, split_path.parent);
    if (!parent)
      return ResultCode::NotFound;

    if (!HasPermission(superblock->fst[*parent], caller_uid, caller_gid, Mode::Read))
      return ResultCode::AccessDenied;

    const Result<u16> child = GetFstIndex(*superblock, *parent, split_path.file_name);
    if (!child)
      return ResultCode::NotFound;
    index = *child;
  }
  else
  {
    return ResultCode::Invalid;
  }

  Metadata metadata;
  metadata.gid = superblock->fst[index].gid;
  metadata.uid = superblock->fst[index].uid;
  metadata.attribute = superblock->fst[index].attr;
  metadata.modes.owner = superblock->fst[index].GetOwnerMode();
  metadata.modes.group = superblock->fst[index].GetGroupMode();
  metadata.modes.other = superblock->fst[index].GetOtherMode();
  metadata.is_file = superblock->fst[index].IsFile();
  metadata.fst_index = index;
  metadata.size = superblock->fst[index].size;
  return metadata;
}

ResultCode NandFileSystem::SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                                       FileAttribute attribute, Modes modes)
{
  if (path.empty() || path.length() > 64 || path[0] != '/')
    return ResultCode::Invalid;

  auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const Result<u16> index = GetFstIndex(*superblock, path);
  if (!index)
    return ResultCode::NotFound;

  FstEntry* current_entry = &superblock->fst[*index];

  if (caller_uid != 0 && caller_uid != current_entry->uid)
    return ResultCode::AccessDenied;

  if (caller_uid != 0 && current_entry->uid != uid)
    return ResultCode::AccessDenied;

  if (current_entry->uid != uid && current_entry->IsFile() && current_entry->size != 0)
    return ResultCode::FileNotEmpty;

  current_entry->gid = gid;
  current_entry->uid = uid;
  current_entry->attr = attribute;
  current_entry->SetAccessMode(modes.owner, modes.group, modes.other);

  return FlushSuperblock();
}

Result<NandStats> NandFileSystem::GetNandStats()
{
  const auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  // XXX: this can be optimised by counting clusters at initialisation time,
  // and updating the counts during file system operations.
  // But generating stat data from the FAT and FST should not take too long
  // on a modern computer anyway -- especially since the data is kept in memory.
  NandStats stats{};

  stats.cluster_size = CLUSTER_DATA_SIZE;
  for (const u16 cluster : superblock->fat)
  {
    switch (cluster)
    {
    case CLUSTER_UNUSED:
    case 0xffff:
      ++stats.free_clusters;
      break;
    case CLUSTER_RESERVED:
      ++stats.reserved_clusters;
      break;
    case CLUSTER_BAD_BLOCK:
      ++stats.bad_clusters;
      break;
    default:
      ++stats.used_clusters;
      break;
    }
  }

  for (const FstEntry& entry : superblock->fst)
  {
    if ((entry.mode & 3) != 0)
      ++stats.used_inodes;
    else
      ++stats.free_inodes;
  }

  if (m_cache_fd != 0xffffffff && m_cache_for_write)
  {
    --stats.free_clusters;
    ++stats.used_clusters;
  }

  return stats;
}

static DirectoryStats CountDirectoryRecursively(const Superblock& superblock, u16 directory)
{
  u32 used_clusters = 0;
  u32 used_inodes = 1;  // one for the directory itself

  const u16 sub = superblock.fst[directory].sub;
  // Traverse the directory
  for (u16 child = sub; child < superblock.fst.size(); child = superblock.fst[child].sib)
  {
    if (superblock.fst[child].IsFile())
    {
      used_clusters += (superblock.fst[child].size + 0x3fff) / 0x4000;
      used_inodes += 1;
    }
    else
    {
      const auto stats_ = CountDirectoryRecursively(superblock, child);
      used_clusters += stats_.used_clusters;
      used_inodes += stats_.used_inodes;
    }
  }
  return {used_clusters, used_inodes};
}

Result<DirectoryStats> NandFileSystem::GetDirectoryStats(const std::string& path)
{
  const auto* superblock = GetSuperblock();
  if (!superblock || path.empty() || path[0] != '/' || path.length() > 64)
    return ResultCode::SuperblockInitFailed;

  const Result<u16> index = GetFstIndex(*superblock, path);
  if (!index)
    return ResultCode::NotFound;

  if (!superblock->fst[*index].IsDirectory())
    return ResultCode::Invalid;

  return CountDirectoryRecursively(*superblock, *index);
}

}  // namespace IOS::HLE::FS
