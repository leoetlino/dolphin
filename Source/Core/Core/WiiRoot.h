// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <functional>

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace Core
{
enum class RestoreReason
{
  EmulationEnd,
  CrashRecovery,
};

void InitializeWiiRoot(bool use_temporary);
void ShutdownWiiRoot();

void BackupWiiSettings();
void RestoreWiiSettings(RestoreReason reason);

// Initialize or clean up the filesystem contents.
void InitializeWiiFileSystemContents();
void CleanUpWiiFileSystemContents();

enum class WiiRootType
{
  Normal,
  /// Temporary roots are deleted when emulation is stopped. Used for NetPlay and Movie.
  Temporary,
};

using WiiFsCallback = std::function<void(IOS::HLE::FS::FileSystem&, WiiRootType)>;
/// Register a one-off callback to be invoked when the Wii file system is being initialised.
void RunOnNextWiiFsInit(WiiFsCallback callback);
/// Register a one-off callback to be invoked when the Wii file system is being cleaned up.
void RunOnNextWiiFsCleanup(WiiFsCallback callback);

}  // namespace Core
