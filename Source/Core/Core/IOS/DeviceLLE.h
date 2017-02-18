// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IPC.h"

namespace IOS
{
namespace LLE
{
void Init();
void Reset();
void Shutdown();
}

namespace HLE
{
namespace Device
{
class LLE : public Device
{
public:
  LLE(u32 device_id, const std::string& device_name);

  IPCCommandResult Open(const Request& request) override;
  IPCCommandResult Close(const Request& request) override;
  IPCCommandResult Read(const ReadWriteRequest& request) override;
  IPCCommandResult Write(const ReadWriteRequest& request) override;
  IPCCommandResult Seek(const SeekRequest& request) override;
  IPCCommandResult IOCtl(const IOCtlRequest& request) override;
  IPCCommandResult IOCtlV(const IOCtlVRequest& request) override;

private:
  void SendRequest(u32 address);

  s32 m_fd = -1;
};
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
