// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#if defined(__linux__) && HAVE_BLUEZ
#include <mutex>
#include <thread>

#include <bluetooth/bluetooth.h>

#include "Common/Flag.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
class WiimoteLinux final : public Wiimote
{
public:
  WiimoteLinux(bdaddr_t bdaddr);
  WiimoteLinux(bdaddr_t bdaddr, int cmd_sock, int int_sock);
  ~WiimoteLinux() override;

protected:
  bool ConnectInternal() override;
  void DisconnectInternal() override;
  bool IsConnected() const override;
  void IOWakeup() override;
  int IORead(u8* buf) override;
  int IOWrite(u8 const* buf, size_t len) override;

private:
  bdaddr_t m_bdaddr;    // Bluetooth address
  int m_cmd_sock = -1;  // Command socket
  int m_int_sock = -1;  // Interrupt socket
  int m_wakeup_pipe_w;
  int m_wakeup_pipe_r;
};

class WiimoteScannerLinux final : public WiimoteScannerBackend
{
public:
  WiimoteScannerLinux();
  ~WiimoteScannerLinux() override;
  bool IsReady() const override;
  void FindWiimotes(std::vector<Wiimote*>&, Wiimote*&) override;
  void Update() override{};  // not needed on Linux

private:
  int m_device_id;
  int m_device_sock;
};

// This scanner listens for incoming connections from Wiimotes,
// instead of _scanning_ for them.
// Unfortunately, this needs cap_net_bind_service and this only works for paired Wiimotes,
// so the normal scanner has to be kept.
class WiimoteScannerLinuxIncoming final : public WiimoteScannerBackend
{
public:
  WiimoteScannerLinuxIncoming();
  ~WiimoteScannerLinuxIncoming() override;
  bool IsReady() const override { return m_thread_running.IsSet(); }
  void FindWiimotes(std::vector<Wiimote*>&, Wiimote*&) override;
  void Update() override{};  // not needed on Linux

private:
  void ListenerThreadFunc();
  Common::Flag m_thread_running;
  std::thread m_thread;
  int m_wakeup_eventfd = -1;

  // Incoming devices are stored here until the scanner calls FindWiimotes.
  std::mutex m_devices_mutex;
  std::vector<Wiimote*> m_found_wiimotes;
  Wiimote* m_found_board = nullptr;

  int m_device_sock = -1;
};
}

#else
#include "Core/HW/WiimoteReal/IODummy.h"
namespace WiimoteReal
{
using WiimoteScannerLinux = WiimoteScannerDummy;
using WiimoteScannerLinuxIncoming = WiimoteScannerDummy;
}
#endif
