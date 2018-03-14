// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/FS/FileSystem.h"

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/FS/ImageBackend/FS.h"

namespace IOS::HLE::FS
{
std::unique_ptr<FileSystem> MakeFileSystem(Location location, IOSC& iosc)
{
  const std::string nand_root =
      File::GetUserPath(location == Location::Session ? D_SESSION_WIIROOT_IDX : D_WIIROOT_IDX);
  const File::FileInfo nand_root_info{nand_root};

  if (nand_root_info.IsDirectory())
    return std::make_unique<HostFileSystem>(nand_root);

  constexpr size_t NAND_SIZE = 0x21000000;
  if (nand_root_info.GetSize() < NAND_SIZE)
  {
    if (nand_root_info.Exists())
    {
      if (!AskYesNoT("Your NAND is corrupted. Delete and recreate it?"))
        return {};
      File::Delete(nand_root);
    }

    if (!File::CreateFullPath(nand_root) || !File::AllocateFile(nand_root, NAND_SIZE))
    {
      PanicAlertT("Failed to create the NAND.");
      return {};
    }
  }

  auto fs = std::make_unique<NandFileSystem>(nand_root, iosc);
  if (nand_root_info.GetSize() < NAND_SIZE && fs->Format(0) != ResultCode::Success)
  {
    PanicAlertT("Failed to format the NAND.");
    return {};
  }
  return fs;
}

IOS::HLE::ReturnCode ConvertResult(ResultCode code)
{
  if (code == ResultCode::Success)
    return IPC_SUCCESS;
  // FS error codes start at -100. Since result codes in the enum are listed in the same way
  // as the IOS codes, we just need to return -100-code.
  return static_cast<ReturnCode>(-(static_cast<s32>(code) + 100));
}

FileHandle::FileHandle(FileSystem* fs, Fd fd) : m_fs{fs}, m_fd{fd}
{
}

FileHandle::FileHandle(FileHandle&& other) : m_fs{other.m_fs}, m_fd{other.m_fd}
{
  other.m_fd.reset();
}

FileHandle& FileHandle::operator=(FileHandle&& other)
{
  if (std::tie(m_fs, m_fd) != std::tie(other.m_fs, other.m_fd))
    *this = std::move(other);
  return *this;
}

FileHandle::~FileHandle()
{
  if (m_fd && m_fs)
    ASSERT(m_fs->Close(*m_fd) == FS::ResultCode::Success);
}

Fd FileHandle::Release()
{
  const Fd fd = m_fd.value();
  m_fd.reset();
  return fd;
}

Result<u32> FileHandle::Seek(u32 offset, SeekMode mode) const
{
  return m_fs->SeekFile(*m_fd, offset, mode);
}

Result<FileStatus> FileHandle::GetStatus() const
{
  return m_fs->GetFileStatus(*m_fd);
}

Result<FileHandle> FileSystem::CreateAndOpenFile(Uid uid, Gid gid, const std::string& path,
                                                 Modes modes)
{
  Result<FileHandle> file = OpenFile(uid, gid, path, Mode::ReadWrite);
  if (file.Succeeded())
    return file;

  const ResultCode result = CreateFile(uid, gid, path, 0, modes);
  if (result != ResultCode::Success)
    return result;

  return OpenFile(uid, gid, path, Mode::ReadWrite);
}

ResultCode FileSystem::CreateFullPath(Uid uid, Gid gid, const std::string& path,
                                      FileAttribute attribute, Modes modes)
{
  std::string::size_type position = 1;
  while (true)
  {
    position = path.find('/', position);
    if (position == std::string::npos)
      return ResultCode::Success;

    const std::string subpath = path.substr(0, position);
    const Result<Metadata> metadata = GetMetadata(uid, gid, subpath);
    if (!metadata && metadata.Error() != ResultCode::NotFound)
      return metadata.Error();
    if (metadata && metadata->is_file)
      return ResultCode::Invalid;

    const ResultCode result = CreateDirectory(uid, gid, subpath, attribute, modes);
    if (result != ResultCode::Success && result != ResultCode::AlreadyExists)
      return result;

    ++position;
  }
}
}  // namespace IOS::HLE::FS
