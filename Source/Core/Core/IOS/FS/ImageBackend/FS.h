// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/ImageBackend/SFFS.h"
#include "Core/IOS/IOSC.h"

namespace IOS::HLE
{
class IOSC;
};

namespace IOS::HLE::FS
{
class NandFileSystem final : public FileSystem
{
public:
  NandFileSystem(const std::string& nand_path, IOSC& iosc);
  ~NandFileSystem();

  void DoState(PointerWrap& p) override;

  ResultCode Format(Uid uid) override;

  Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) override;
  ResultCode Close(Fd fd) override;
  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) override;
  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) override;
  Result<u32> SeekFile(Fd fd, std::uint32_t offset, SeekMode mode) override;
  Result<FileStatus> GetFileStatus(Fd fd) override;

  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes) override;

  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes) override;

  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path) override;

  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path) override;

  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes) override;

  Result<NandStats> GetNandStats() override;
  Result<DirectoryStats> GetDirectoryStats(const std::string& path) override;

private:
  struct Handle
  {
    bool opened = false;
    u16 fst_index = 0xffff;
    u16 gid = 0;
    u32 uid = 0;
    Mode mode = Mode::None;
    u32 file_offset = 0;
    u32 file_size = 0;
    bool superblock_flush_needed = false;
  };
  Handle* AssignFreeHandle(Uid uid, Gid gid);
  Handle* GetHandleFromFd(Fd fd);
  Fd ConvertHandleToFd(const Handle* handle) const;

  /// Check if a file has been opened.
  bool IsFileOpened(u16 fst_index) const;
  /// Recursively check if any file in a directory has been opened.
  /// A valid directory FST index must be passed.
  bool IsDirectoryInUse(const Superblock& superblock, u16 directory_index) const;

  ResultCode CreateFileOrDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                                   FileAttribute attribute, Modes modes, bool is_file);

  IOSC::Hash GenerateHmacForSuperblock(const Superblock& superblock, u16 superblock_index);
  /// cluster_data *must* point to a 0x4000 bytes long buffer.
  IOSC::Hash GenerateHmacForData(const Superblock& superblock, const u8* cluster_data,
                                 u16 fst_index, u16 chain_index);

  /// Read a cluster (0x4000 bytes).
  Result<std::array<IOSC::Hash, 2>> ReadCluster(u16 cluster, u8* data);
  /// Read a cluster for a file (0x4000 bytes).
  ResultCode ReadFileData(u16 fst_index, u16 chain_index, u8* data);
  Result<Superblock> ReadSuperblock(u16 superblock);
  Superblock* GetSuperblock();
  Result<u16> GetFstIndex(const Superblock& superblock, const std::string& path) const;
  Result<u16> GetFstIndex(const Superblock& superblock, u16 parent, const std::string& file) const;
  Result<u16> GetUnusedFstIndex(const Superblock& superblock) const;

  /// Write 0x4000 bytes of data to the NAND.
  ResultCode WriteCluster(u16 cluster, const u8* data, const IOSC::Hash& hmac);
  ResultCode WriteFileData(u16 fst_index, const u8* data, u16 chain_index, u32 new_size);
  /// Write a new superblock to the NAND to persist changes that were made to metadata.
  ResultCode FlushSuperblock();

  /// Flush the file cache.
  ResultCode FlushFileCache();
  /// Populate the file cache.
  ResultCode PopulateFileCache(Handle* handle, u32 offset, bool write);

  IOSC& m_iosc;
  File::IOFile m_nand;
  std::string m_nand_path;
  IOSC::BlockMacGenerator m_block_mac_generator;
  std::unique_ptr<Superblock> m_superblock;
  u32 m_superblock_index = 0;
  std::array<Handle, 16> m_handles{};

  // Store FDs instead of handle pointers to be more savestate friendly.
  Fd m_cache_fd = 0xffffffff;
  u16 m_cache_chain_index = 0xffff;
  std::array<u8, CLUSTER_DATA_SIZE> m_cache_data;
  bool m_cache_for_write = false;
};

}  // namespace IOS::HLE::FS
