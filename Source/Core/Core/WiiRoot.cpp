// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/WiiRoot.h"

#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/SysConf.h"

namespace Core
{
static std::string s_temp_wii_root;

void InitializeWiiRoot(bool use_temporary)
{
  if (use_temporary)
  {
    s_temp_wii_root = File::CreateTempDir();
    if (s_temp_wii_root.empty())
    {
      ERROR_LOG(IOS_FS, "Could not create temporary directory");
      return;
    }
    WARN_LOG(IOS_FS, "Using temporary directory %s for minimal Wii FS", s_temp_wii_root.c_str());
    File::SetUserPath(D_SESSION_WIIROOT_IDX, s_temp_wii_root);
  }
  else
  {
    File::SetUserPath(D_SESSION_WIIROOT_IDX, File::GetUserPath(D_WIIROOT_IDX));
  }
}

void ShutdownWiiRoot()
{
  if (!s_temp_wii_root.empty())
  {
    File::DeleteDirRecursively(s_temp_wii_root);
    s_temp_wii_root.clear();
  }
}

using namespace IOS::HLE::FS;

static bool CopyToAnotherNand(FileSystem* source, FileSystem* dest, const std::string& path,
                              Uid uid, Gid gid)
{
  const auto metadata = source->GetMetadata(0, 0, path);
  if (!metadata)
    return false;

  // Create the file or the directory and copy the access modes.
  if (metadata->is_file)
    dest->CreateFile(0, 0, path, 0, Mode::None, Mode::None, Mode::None);
  else
    dest->CreateDirectory(0, 0, path, 0, Mode::None, Mode::None, Mode::None);

  dest->SetMetadata(0, path, uid, gid, 0, metadata->owner_mode, metadata->group_mode,
                    metadata->other_mode);

  if (metadata->is_file)
  {
    std::vector<u8> file(metadata->size);
    const auto fd = source->OpenFile(0, 0, path, Mode::Read);
    if (!fd || !source->ReadFile(*fd, file.data(), file.size()))
      return false;

    const auto dest_fd = dest->OpenFile(0, 0, path, Mode::Write);
    return dest_fd && dest->WriteFile(*dest_fd, file.data(), file.size());
  }

  const auto entries = source->ReadDirectory(0, 0, path);
  if (!entries)
    return false;

  for (const std::string& child : *entries)
  {
    if (!CopyToAnotherNand(source, dest, path + "/" + child, uid, gid))
      return false;
  }
  return true;
}

static void InitializeTitleDirectories(FileSystem* fs)
{
  const std::string sysmenu_data_directory = Common::GetTitleDataPath(Titles::SYSTEM_MENU);
  if (fs->ReadDirectory(0, 0, sysmenu_data_directory).Succeeded())
    return;

  fs->CreateFullPath(0, 0, sysmenu_data_directory, 0, Mode::ReadWrite, Mode::ReadWrite, Mode::Read);
  fs->CreateDirectory(0, 0, sysmenu_data_directory, 0, Mode::ReadWrite, Mode::None, Mode::None);
  fs->SetMetadata(0, sysmenu_data_directory, 0x1000, 1, 0, Mode::ReadWrite, Mode::None, Mode::None);
}

static void InitializeWiiConnect24(FileSystem* fs)
{
  HostFileSystem initial_fs{File::GetSysDirectory() + WII_USER_DIR};
  // /shared2/wc24 contents are normally created by the system menu.
  CopyToAnotherNand(&initial_fs, fs, "/shared2/wc24", 0x1000, 1);
}

static void InitializeDeterministicWiiSaves(FileSystem* fs)
{
  if (s_temp_wii_root.empty())
    return;

  const std::unique_ptr<FileSystem> user_fs = MakeFileSystem(Location::Configured);

  const u64 title_id = SConfig::GetInstance().GetTitleID();
  const std::string save_path = Common::GetTitleDataPath(title_id);
  if (Movie::IsRecordingInput())
  {
    // TODO: Check for the actual save data
    if (NetPlay::IsNetPlayRunning() && !SConfig::GetInstance().bCopyWiiSaveNetplay)
      Movie::SetClearSave(true);
    else
      Movie::SetClearSave(!user_fs->GetMetadata(0, 0, save_path + "/banner.bin").Succeeded());
  }

  if ((NetPlay::IsNetPlayRunning() && SConfig::GetInstance().bCopyWiiSaveNetplay) ||
      (Movie::IsMovieActive() && !Movie::IsStartingFromClearSave()))
  {
    // Copy the current user's save to the Blank NAND
    if (const auto metadata = user_fs->GetMetadata(0, 0, save_path + "/banner.bin"))
    {
      IOS::ES::UIDSys uid_map{fs};
      const u32 uid_in_temp_fs = uid_map.GetOrInsertUIDForTitle(title_id);
      CopyToAnotherNand(user_fs.get(), fs, save_path, uid_in_temp_fs, metadata->gid);
    }
  }
}

void InitializeFS()
{
  const auto fs = IOS::HLE::GetIOS()->GetFS();

  InitializeTitleDirectories(fs.get());
  InitializeWiiConnect24(fs.get());
  InitializeDeterministicWiiSaves(fs.get());

  SysConf sysconf{fs.get()};
  sysconf.Save();
}

void ShutdownFS()
{
  if (s_temp_wii_root.empty())
    return;

  const std::unique_ptr<FileSystem> user_fs = MakeFileSystem(Location::Configured);
  const std::shared_ptr<FileSystem> temp_fs = IOS::HLE::GetIOS()->GetFS();

  const u64 title_id = SConfig::GetInstance().GetTitleID();
  const std::string title_path = Common::GetTitlePath(title_id);
  if (!temp_fs->GetMetadata(0, 0, title_path + "/data/banner.bin").Succeeded() ||
      !SConfig::GetInstance().bEnableMemcardSdWriting)
  {
    return;
  }

  // Backup the existing save just in case it's still needed.
  if (user_fs->GetMetadata(0, 0, title_path + "/data/banner.bin"))
  {
    HostFileSystem backup_fs{File::GetUserPath(D_BACKUP_IDX)};
    backup_fs.CreateFullPath(0, 0, title_path, 0, Mode::ReadWrite, Mode::ReadWrite, Mode::Read);
    CopyToAnotherNand(temp_fs.get(), &backup_fs, title_path, 0, 0);
  }

  // Copy the save back to the configured NAND with the proper metadata
  IOS::ES::UIDSys uid_map{user_fs.get()};
  const u32 uid = uid_map.GetOrInsertUIDForTitle(title_id);
  const u16 gid = IOS::HLE::GetIOS()->GetES()->FindInstalledTMD(title_id).GetGroupId();

  user_fs->CreateFullPath(0, 0, title_path + "/", 0, Mode::ReadWrite, Mode::ReadWrite, Mode::Read);
  CopyToAnotherNand(temp_fs.get(), user_fs.get(), title_path, uid, gid);
  user_fs->SetMetadata(0, title_path + "/", 0, 0, 0, Mode::ReadWrite, Mode::ReadWrite, Mode::Read);
  user_fs->SetMetadata(0, title_path + "/content", 0, 0, 0, Mode::ReadWrite, Mode::ReadWrite,
                       Mode::None);
  user_fs->SetMetadata(0, title_path + "/data", 0, uid, gid, Mode::ReadWrite, Mode::None,
                       Mode::None);
}
}
