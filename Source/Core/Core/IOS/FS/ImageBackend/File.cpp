// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>

#include "Common/Logging/Log.h"
#include "Core/IOS/FS/ImageBackend/FS.h"
#include "Core/IOS/FS/ImageBackend/Util.h"

namespace IOS::HLE::FS
{
Result<FileHandle> NandFileSystem::OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode)
{
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;

  const auto* superblock = GetSuperblock();
  if (!superblock)
    return ResultCode::SuperblockInitFailed;

  const Result<u16> index = GetFstIndex(*superblock, path);
  if (!index)
    return ResultCode::NotFound;

  if (!superblock->fst[*index].IsFile())
    return ResultCode::Invalid;

  if (!HasPermission(superblock->fst[*index], uid, gid, mode))
    return ResultCode::AccessDenied;

  Handle* handle = AssignFreeHandle(uid, gid);
  if (!handle)
    return ResultCode::NoFreeHandle;
  handle->fst_index = *index;
  handle->mode = mode;
  handle->file_offset = 0;
  // For one handle, the file size is stored once and never touched again except for writes.
  // This means that if the same file is opened twice, and the second handle is used to
  // grow the file, the first handle will not be able to read past the original size.
  handle->file_size = superblock->fst[*index].size;
  return FileHandle{this, ConvertHandleToFd(handle)};
}

ResultCode NandFileSystem::PopulateFileCache(Handle* handle, u32 offset, bool write)
{
  const u16 chain_index = offset / CLUSTER_DATA_SIZE;
  if (m_cache_fd == ConvertHandleToFd(handle) && m_cache_chain_index == chain_index)
    return ResultCode::Success;

  const auto flush_result = FlushFileCache();
  if (flush_result != ResultCode::Success)
    return flush_result;

  if (write)
  {
    const auto superblock = GetSuperblock();
    if (!superblock)
      return ResultCode::SuperblockInitFailed;

    const auto it = std::find(superblock->fat.begin(), superblock->fat.end(), CLUSTER_UNUSED);
    if (it == superblock->fat.end())
      return ResultCode::NoFreeSpace;
  }

  if (offset % CLUSTER_DATA_SIZE != 0 || offset != handle->file_size)
  {
    const ResultCode result = ReadFileData(handle->fst_index, chain_index, m_cache_data.data());
    if (result != ResultCode::Success)
    {
      ERROR_LOG(IOS_FS, "Failed to read data into cache: error %d", ConvertResult(result));
      return result;
    }
  }

  m_cache_fd = ConvertHandleToFd(handle);
  m_cache_chain_index = chain_index;
  m_cache_for_write = write;
  return ResultCode::Success;
}

ResultCode NandFileSystem::FlushFileCache()
{
  if (m_cache_fd == 0xffffffff || !m_cache_for_write || m_cache_data.size() != CLUSTER_DATA_SIZE)
    return ResultCode::Success;

  Handle* handle = GetHandleFromFd(m_cache_fd);
  const auto result =
      WriteFileData(handle->fst_index, m_cache_data.data(), m_cache_chain_index, handle->file_size);
  if (result == ResultCode::Success)
    handle->superblock_flush_needed = true;
  else
    ERROR_LOG(IOS_FS, "Failed to flush file cache %u: error %d", m_cache_fd, ConvertResult(result));
  return result;
}

ResultCode NandFileSystem::Close(Fd fd)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle)
    return ResultCode::Invalid;

  if (m_cache_fd == fd)
  {
    const auto flush_result = FlushFileCache();
    if (flush_result != ResultCode::Success)
      return flush_result;

    m_cache_fd = 0xffffffff;
  }

  if (handle->superblock_flush_needed)
  {
    const auto result = FlushSuperblock();
    if (result != ResultCode::Success)
      return result;
  }

  *handle = Handle{};
  return ResultCode::Success;
}

Result<u32> NandFileSystem::ReadBytesFromFile(Fd fd, u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || handle->fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  if ((u8(handle->mode) & u8(Mode::Read)) == 0)
    return ResultCode::AccessDenied;

  if (count + handle->file_offset > handle->file_size)
    count = handle->file_size - handle->file_offset;

  u32 processed_count = 0;
  while (processed_count != count)
  {
    const auto result = PopulateFileCache(handle, handle->file_offset, false);
    if (result != ResultCode::Success)
      return result;

    const auto start =
        m_cache_data.begin() + (handle->file_offset - m_cache_chain_index * CLUSTER_DATA_SIZE);
    const size_t copy_length =
        std::min<size_t>(m_cache_data.end() - start, count - processed_count);

    std::copy_n(start, copy_length, ptr + processed_count);
    handle->file_offset += copy_length;
    processed_count += copy_length;
  }
  return count;
}

Result<u32> NandFileSystem::WriteBytesToFile(Fd fd, const u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || handle->fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  if ((u8(handle->mode) & u8(Mode::Write)) == 0)
    return ResultCode::AccessDenied;

  u32 processed_count = 0;
  while (processed_count != count)
  {
    const auto result = PopulateFileCache(handle, handle->file_offset, true);
    if (result != ResultCode::Success)
      return result;

    const auto start =
        m_cache_data.begin() + (handle->file_offset - m_cache_chain_index * CLUSTER_DATA_SIZE);
    const size_t copy_length =
        std::min<size_t>(m_cache_data.end() - start, count - processed_count);

    std::copy_n(ptr + processed_count, copy_length, start);
    handle->file_offset += copy_length;
    processed_count += copy_length;
    handle->file_size = std::max(handle->file_offset, handle->file_size);
  }
  return count;
}

Result<u32> NandFileSystem::SeekFile(Fd fd, std::uint32_t offset, SeekMode mode)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || handle->fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  u32 new_position = 0;
  switch (mode)
  {
  case SeekMode::Set:
    new_position = offset;
    break;
  case SeekMode::Current:
    new_position = handle->file_offset + offset;
    break;
  case SeekMode::End:
    new_position = handle->file_size + offset;
    break;
  default:
    return ResultCode::Invalid;
  }

  // This differs from POSIX behaviour which allows seeking past the end of the file.
  if (handle->file_size < new_position)
    return ResultCode::Invalid;

  handle->file_offset = new_position;
  return handle->file_offset;
}

Result<FileStatus> NandFileSystem::GetFileStatus(Fd fd)
{
  const Handle* handle = GetHandleFromFd(fd);
  if (!handle || handle->fst_index >= std::tuple_size<decltype(Superblock::fst)>::value)
    return ResultCode::Invalid;

  FileStatus status;
  status.size = handle->file_size;
  status.offset = handle->file_offset;
  return status;
}

NandFileSystem::Handle* NandFileSystem::AssignFreeHandle(Uid uid, Gid gid)
{
  const auto it = std::find_if(m_handles.begin(), m_handles.end(),
                               [](const Handle& handle) { return !handle.opened; });
  if (it == m_handles.end())
    return nullptr;

  *it = Handle{};
  it->opened = true;
  it->uid = uid;
  it->gid = gid;
  return &*it;
}

NandFileSystem::Handle* NandFileSystem::GetHandleFromFd(Fd fd)
{
  if (fd >= m_handles.size() || !m_handles[fd].opened)
    return nullptr;
  return &m_handles[fd];
}

Fd NandFileSystem::ConvertHandleToFd(const Handle* handle) const
{
  return handle - m_handles.data();
}

bool NandFileSystem::IsFileOpened(u16 fst_index) const
{
  return std::any_of(m_handles.begin(), m_handles.end(), [fst_index](const Handle& handle) {
    return handle.opened && handle.fst_index == fst_index;
  });
}

bool NandFileSystem::IsDirectoryInUse(const Superblock& superblock, u16 directory) const
{
  const u16 sub = superblock.fst[directory].sub;
  // Traverse the directory
  for (u16 child = sub; child < superblock.fst.size(); child = superblock.fst[child].sib)
  {
    if (superblock.fst[child].IsFile())
    {
      if (IsFileOpened(child))
        return true;
    }
    else
    {
      if (IsDirectoryInUse(superblock, child))
        return true;
    }
  }
  return false;
}

}  // namespace IOS::HLE::FS
