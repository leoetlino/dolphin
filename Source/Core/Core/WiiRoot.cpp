// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/WiiRoot.h"

#include <cinttypes>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/SysConf.h"

namespace Core
{
namespace FS = IOS::HLE::FS;

static std::string s_temp_wii_root;
static std::vector<WiiFsCallback> s_fs_init_callbacks;
static std::vector<WiiFsCallback> s_fs_cleanup_callbacks;

static bool CopyBackupFile(const std::string& path_from, const std::string& path_to)
{
  if (!File::Exists(path_from))
    return false;

  File::CreateFullPath(path_to);

  return File::Copy(path_from, path_to);
}

static void DeleteBackupFile(const std::string& file_name)
{
  File::Delete(File::GetUserPath(D_BACKUP_IDX) + DIR_SEP + file_name);
}

static void BackupFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + DIR_SEP + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + DIR_SEP + file_name;

  CopyBackupFile(original_path, backup_path);
}

static void RestoreFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + DIR_SEP + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + DIR_SEP + file_name;

  if (CopyBackupFile(backup_path, original_path))
    DeleteBackupFile(file_name);
}

static void InitializeDeterministicWiiSaves(FS::FileSystem* session_fs)
{
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  const auto configured_fs = FS::MakeFileSystem(FS::Location::Configured);

  if (NetPlay::IsNetPlayRunning() && SConfig::GetInstance().bCopyWiiSaveNetplay)
  {
    // Copy the current user's save to the Blank NAND
    auto* sync_fs = NetPlay::GetWiiSyncFS();
    auto& sync_titles = NetPlay::GetWiiSyncTitles();
    if (sync_fs)
    {
      for (const u64 title : sync_titles)
      {
        WiiSave::Copy(sync_fs, session_fs, title);
      }

      // Copy Mii data
      if (!FS::CopyFile(sync_fs, Common::GetMiiDatabasePath(), session_fs,
                        Common::GetMiiDatabasePath()))
      {
        WARN_LOG(CORE, "Failed to copy Mii database to the NAND");
      }
    }
    else
    {
      if (NetPlay::IsSyncingAllWiiSaves())
      {
        for (const u64 title : sync_titles)
        {
          WiiSave::Copy(configured_fs.get(), session_fs, title);
        }
      }
      else
      {
        WiiSave::Copy(configured_fs.get(), session_fs, title_id);
      }

      // Copy Mii data
      if (!FS::CopyFile(configured_fs.get(), Common::GetMiiDatabasePath(), session_fs,
                        Common::GetMiiDatabasePath()))
      {
        WARN_LOG(CORE, "Failed to copy Mii database to the NAND");
      }
    }
  }
}

void InitializeWiiRoot(bool use_temporary)
{
  if (use_temporary)
  {
    s_temp_wii_root = File::GetUserPath(D_USER_IDX) + "WiiSession" DIR_SEP;
    WARN_LOG(IOS_FS, "Using temporary directory %s for minimal Wii FS", s_temp_wii_root.c_str());

    // If directory exists, make a backup
    if (File::Exists(s_temp_wii_root))
    {
      const std::string backup_path =
          s_temp_wii_root.substr(0, s_temp_wii_root.size() - 1) + ".backup" DIR_SEP;
      WARN_LOG(IOS_FS, "Temporary Wii FS directory exists, moving to backup...");

      // If backup exists, delete it as we don't want a mess
      if (File::Exists(backup_path))
      {
        WARN_LOG(IOS_FS, "Temporary Wii FS backup directory exists, deleting...");
        File::DeleteDirRecursively(backup_path);
      }

      File::CopyDir(s_temp_wii_root, backup_path, true);
    }

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

void BackupWiiSettings()
{
  // Back up files which Dolphin can modify at boot, so that we can preserve the original contents.
  // For SYSCONF, the backup is only needed in case Dolphin crashes or otherwise exists unexpectedly
  // during emulation, since the config system will restore the SYSCONF settings at emulation end.
  // For setting.txt, there is no other code that restores the original values for us.

  BackupFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);
  BackupFile("/shared2/sys/SYSCONF");
}

void RestoreWiiSettings(RestoreReason reason)
{
  RestoreFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);

  // We must not restore the SYSCONF backup when ending emulation cleanly, since the user may have
  // edited the SYSCONF file in the NAND using the emulated software (e.g. the Wii Menu settings).
  if (reason == RestoreReason::CrashRecovery)
    RestoreFile("/shared2/sys/SYSCONF");
  else
    DeleteBackupFile("SYSCONF");
}

/// Copy a directory from host_source_path (on the host FS) to nand_target_path on the NAND.
///
/// Both paths should not have trailing slashes. To specify the NAND root, use "".
static bool CopySysmenuFilesToFS(FS::FileSystem* fs, const std::string& host_source_path,
                                 const std::string& nand_target_path)
{
  const auto entries = File::ScanDirectoryTree(host_source_path, false);
  for (const File::FSTEntry& entry : entries.children)
  {
    const std::string host_path = host_source_path + '/' + entry.virtualName;
    const std::string nand_path = nand_target_path + '/' + entry.virtualName;

    if (entry.isDirectory)
    {
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, 0, FS::WideOpenModes);
      if (!CopySysmenuFilesToFS(fs, host_path, nand_path))
        return false;
    }
    else
    {
      // Do not overwrite any existing files.
      if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path).Succeeded())
        continue;

      File::IOFile host_file{host_path, "rb"};
      std::vector<u8> file_data(host_file.GetSize());
      if (!host_file.ReadBytes(file_data.data(), file_data.size()))
        return false;

      const auto nand_file =
          fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, FS::WideOpenModes);
      if (!nand_file || !nand_file->Write(file_data.data(), file_data.size()))
        return false;
    }
  }
  return true;
}

void InitializeWiiFileSystemContents()
{
  const auto fs = IOS::HLE::GetIOS()->GetFS();

  // Some games (such as Mario Kart Wii) assume that NWC24 files will always be present
  // even upon the first launch as they are normally created by the system menu.
  // Because we do not require the system menu to be run, WiiConnect24 files must be copied
  // to the NAND manually.
  if (!CopySysmenuFilesToFS(fs.get(), File::GetSysDirectory() + WII_USER_DIR, ""))
    WARN_LOG(CORE, "Failed to copy initial System Menu files to the NAND");

  const WiiRootType type = s_temp_wii_root.empty() ? WiiRootType::Normal : WiiRootType::Temporary;

  if (type == WiiRootType::Temporary)
  {
    // Generate a SYSCONF with default settings for the temporary Wii NAND.
    SysConf sysconf{fs};
    sysconf.Save();

    InitializeDeterministicWiiSaves(fs.get());
  }

  for (const auto& callback : s_fs_init_callbacks)
    callback(*fs, type);
  s_fs_init_callbacks.clear();
}

void CleanUpWiiFileSystemContents()
{
  const WiiRootType type = s_temp_wii_root.empty() ? WiiRootType::Normal : WiiRootType::Temporary;

  for (const auto& callback : s_fs_cleanup_callbacks)
    callback(*IOS::HLE::GetIOS()->GetFS(), type);
  s_fs_cleanup_callbacks.clear();
}

void RunOnNextWiiFsInit(WiiFsCallback callback)
{
  s_fs_init_callbacks.emplace_back(std::move(callback));
}

void RunOnNextWiiFsCleanup(WiiFsCallback callback)
{
  s_fs_cleanup_callbacks.emplace_back(std::move(callback));
}

}  // namespace Core
