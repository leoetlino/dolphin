// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/Network/NCD/WiiNetConfig.h"

#include <cstring>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"

namespace IOS
{
namespace HLE
{
namespace Net
{
static const std::string CONFIG_PATH = "/shared2/sys/net/02/config.dat";

WiiNetConfig::WiiNetConfig() = default;

void WiiNetConfig::ReadConfig(FS::FileSystem* fs)
{
  const auto fd = fs->OpenFile(PID_NCD, PID_NCD, CONFIG_PATH, FS::Mode::Read);
  if (!fd || !fs->ReadFile(*fd, &m_data, 1))
    ResetConfig(fs);
}

void WiiNetConfig::WriteConfig(FS::FileSystem* fs) const
{
  fs->CreateFullPath(PID_NCD, PID_NCD, CONFIG_PATH, 0, FS::Mode::ReadWrite, FS::Mode::ReadWrite,
                     FS::Mode::ReadWrite);
  fs->CreateFile(PID_NCD, PID_NCD, CONFIG_PATH, 0, FS::Mode::ReadWrite, FS::Mode::ReadWrite,
                 FS::Mode::ReadWrite);
  const auto fd = fs->OpenFile(PID_NCD, PID_NCD, CONFIG_PATH, FS::Mode::Write);
  if (!fd || !fs->WriteFile(*fd, &m_data, 1))
    ERROR_LOG(IOS_WC24, "Failed to write config");
}

void WiiNetConfig::ResetConfig(FS::FileSystem* fs)
{
  fs->Delete(PID_NCD, PID_NCD, CONFIG_PATH);

  memset(&m_data, 0, sizeof(m_data));
  m_data.connType = ConfigData::IF_WIRED;
  m_data.connection[0].flags =
      ConnectionSettings::WIRED_IF | ConnectionSettings::DNS_DHCP | ConnectionSettings::IP_DHCP |
      ConnectionSettings::CONNECTION_TEST_OK | ConnectionSettings::CONNECTION_SELECTED;

  WriteConfig(fs);
}

void WiiNetConfig::WriteToMem(const u32 address) const
{
  Memory::CopyToEmu(address, &m_data, sizeof(m_data));
}

void WiiNetConfig::ReadFromMem(const u32 address)
{
  Memory::CopyFromEmu(&m_data, address, sizeof(m_data));
}
}  // namespace Net
}  // namespace HLE
}  // namespace IOS
