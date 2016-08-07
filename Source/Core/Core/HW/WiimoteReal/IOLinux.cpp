// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteReal/IOLinux.h"

namespace WiimoteReal
{
// This is used to store the Bluetooth address of connected Wiimotes,
// so we can ignore Wiimotes that are already connected.
static std::vector<std::string> s_known_addrs;
static bool IsNewWiimote(const std::string& addr)
{
  return std::find(s_known_addrs.begin(), s_known_addrs.end(), addr) == s_known_addrs.end();
}

WiimoteScannerLinux::WiimoteScannerLinux() : m_device_id(-1), m_device_sock(-1)
{
  // Get the id of the first Bluetooth device.
  m_device_id = hci_get_route(nullptr);
  if (m_device_id < 0)
  {
    NOTICE_LOG(WIIMOTE, "Bluetooth not found.");
    return;
  }

  // Create a socket to the device
  m_device_sock = hci_open_dev(m_device_id);
  if (m_device_sock < 0)
  {
    ERROR_LOG(WIIMOTE, "Unable to open Bluetooth.");
    return;
  }
}

WiimoteScannerLinux::~WiimoteScannerLinux()
{
  if (IsReady())
    close(m_device_sock);
}

bool WiimoteScannerLinux::IsReady() const
{
  return m_device_sock > 0;
}

void WiimoteScannerLinux::FindWiimotes(std::vector<Wiimote*>& found_wiimotes, Wiimote*& found_board)
{
  // supposedly 1.28 seconds
  int const wait_len = 1;

  int const max_infos = 255;
  inquiry_info scan_infos[max_infos] = {};
  auto* scan_infos_ptr = scan_infos;
  found_board = nullptr;
  // Use Limited Dedicated Inquiry Access Code (LIAC) to query, since third-party Wiimotes
  // cannot be discovered without it.
  const u8 lap[3] = {0x00, 0x8b, 0x9e};

  // Scan for Bluetooth devices
  int const found_devices =
      hci_inquiry(m_device_id, wait_len, max_infos, lap, &scan_infos_ptr, IREQ_CACHE_FLUSH);
  if (found_devices < 0)
  {
    ERROR_LOG(WIIMOTE, "Error searching for Bluetooth devices.");
    return;
  }

  DEBUG_LOG(WIIMOTE, "Found %i Bluetooth device(s).", found_devices);

  // Display discovered devices
  for (int i = 0; i < found_devices; ++i)
  {
    ERROR_LOG(WIIMOTE, "found a device...");

    // BT names are a maximum of 248 bytes apparently
    char name[255] = {};
    if (hci_read_remote_name(m_device_sock, &scan_infos[i].bdaddr, sizeof(name), name, 1000) < 0)
    {
      ERROR_LOG(WIIMOTE, "name request failed");
      continue;
    }

    ERROR_LOG(WIIMOTE, "device name %s", name);
    if (!IsValidBluetoothName(name))
      continue;

    char bdaddr_str[18] = {};
    ba2str(&scan_infos[i].bdaddr, bdaddr_str);

    if (!IsNewWiimote(bdaddr_str))
      continue;

    // Found a new device
    s_known_addrs.push_back(bdaddr_str);
    Wiimote* wm = new WiimoteLinux(scan_infos[i].bdaddr);
    if (IsBalanceBoardName(name))
    {
      found_board = wm;
      NOTICE_LOG(WIIMOTE, "Found balance board (%s).", bdaddr_str);
    }
    else
    {
      found_wiimotes.push_back(wm);
      NOTICE_LOG(WIIMOTE, "Found Wiimote (%s).", bdaddr_str);
    }
  }
}

WiimoteScannerLinuxIncoming::WiimoteScannerLinuxIncoming()
{
  if (m_thread_running.IsSet())
    return;

  const int device_id = hci_get_route(nullptr);
  if (device_id < 0)
    return;
  m_device_sock = hci_open_dev(device_id);
  if (m_device_sock == -1)
  {
    ERROR_LOG(WIIMOTE, "Unable to open Bluetooth device.");
    return;
  }

  m_wakeup_eventfd = eventfd(0, 0);
  _assert_msg_(WIIMOTE, m_wakeup_eventfd != -1, "Couldn't create eventfd.");
  m_thread_running.Set();
  m_thread = std::thread(&WiimoteScannerLinuxIncoming::ListenerThreadFunc, this);
}

WiimoteScannerLinuxIncoming::~WiimoteScannerLinuxIncoming()
{
  if (m_thread_running.TestAndClear())
  {
    // Write something to efd so that select() stops blocking.
    uint64_t value = 1;
    write(m_wakeup_eventfd, &value, sizeof(uint64_t));
    m_thread.join();
  }
  if (m_device_sock != -1)
    close(m_device_sock);
}

void WiimoteScannerLinuxIncoming::ListenerThreadFunc()
{
  // Input channel
  sockaddr_l2 addr;
  addr.l2_family = AF_BLUETOOTH;
  addr.l2_psm = htobs(WM_INPUT_CHANNEL);
  const int int_listen_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
  if (bind(int_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
  {
    WARN_LOG(WIIMOTE, "Failed to listen for incoming connections from Wiimotes "
                      "(likely a permission issue). This feature will be disabled.");
    return;
  }
  listen(int_listen_fd, 1);
  // Output channel
  addr.l2_psm = htobs(WM_OUTPUT_CHANNEL);
  const int cmd_listen_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
  bind(cmd_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  listen(cmd_listen_fd, 1);

  while (m_thread_running.IsSet())
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_wakeup_eventfd, &fds);
    FD_SET(int_listen_fd, &fds);
    FD_SET(cmd_listen_fd, &fds);

    const int ret = select(cmd_listen_fd + 1, &fds, nullptr, nullptr, nullptr);
    if (ret < 1 || FD_ISSET(m_wakeup_eventfd, &fds))
      continue;

    sockaddr_l2 client_addr;
    socklen_t len = sizeof(client_addr);
    const int cmd_sock = accept(cmd_listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
    const int int_sock = accept(int_listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);

    char name[255] = {};
    if (hci_read_remote_name(m_device_sock, &client_addr.l2_bdaddr, sizeof(name), name, 1000) < 0)
    {
      ERROR_LOG(WIIMOTE, "Failed to request name, ignoring incoming connection");
      close(cmd_sock);
      close(int_sock);
      continue;
    }

    if (IsValidBluetoothName(name))
    {
      std::lock_guard<std::mutex> lk(m_devices_mutex);
      char bdaddr_str[18] = {};
      ba2str(&client_addr.l2_bdaddr, bdaddr_str);
      if (IsBalanceBoardName(name))
      {
        NOTICE_LOG(WIIMOTE, "Detected a Balance Board incoming connection (%s)", bdaddr_str);
        if (m_found_board != nullptr)
          delete m_found_board;
        m_found_board = new WiimoteLinux(client_addr.l2_bdaddr, cmd_sock, int_sock);
      }
      else
      {
        NOTICE_LOG(WIIMOTE, "Detected a Wiimote incoming connection (%s)", bdaddr_str);
        m_found_wiimotes.emplace_back(new WiimoteLinux(client_addr.l2_bdaddr, cmd_sock, int_sock));
      }
    }
  }
  close(int_listen_fd);
  close(cmd_listen_fd);
}

void WiimoteScannerLinuxIncoming::FindWiimotes(std::vector<Wiimote*>& wiimotes, Wiimote*& board)
{
  std::lock_guard<std::mutex> lk(m_devices_mutex);

  for (auto* wiimote : m_found_wiimotes)
    wiimotes.emplace_back(wiimote);
  m_found_wiimotes.clear();

  board = m_found_board;
  m_found_board = nullptr;
}

WiimoteLinux::WiimoteLinux(bdaddr_t bdaddr) : Wiimote(), m_bdaddr(bdaddr)
{
  m_really_disconnect = true;

  int fds[2];
  if (pipe(fds))
  {
    ERROR_LOG(WIIMOTE, "pipe failed");
    abort();
  }
  m_wakeup_pipe_w = fds[1];
  m_wakeup_pipe_r = fds[0];
}

WiimoteLinux::WiimoteLinux(bdaddr_t bdaddr, int cmd_sock, int int_sock) : WiimoteLinux(bdaddr)
{
  m_cmd_sock = cmd_sock;
  m_int_sock = int_sock;
}

WiimoteLinux::~WiimoteLinux()
{
  Shutdown();
  close(m_wakeup_pipe_w);
  close(m_wakeup_pipe_r);
}

// Connect to a Wiimote with a known address.
bool WiimoteLinux::ConnectInternal()
{
  if (m_int_sock != -1 && m_cmd_sock != -1)
    return true;

  sockaddr_l2 addr = {};
  addr.l2_family = AF_BLUETOOTH;
  addr.l2_bdaddr = m_bdaddr;
  addr.l2_cid = 0;

  // Output channel
  addr.l2_psm = htobs(WM_OUTPUT_CHANNEL);
  if ((m_cmd_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) == -1 ||
      connect(m_cmd_sock, (sockaddr*)&addr, sizeof(addr)) < 0)
  {
    WARN_LOG(WIIMOTE, "Unable to open output socket to Wiimote: %s", strerror(errno));
    close(m_cmd_sock);
    m_cmd_sock = -1;
    return false;
  }

  // Input channel
  addr.l2_psm = htobs(WM_INPUT_CHANNEL);
  if ((m_int_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) == -1 ||
      connect(m_int_sock, (sockaddr*)&addr, sizeof(addr)) < 0)
  {
    WARN_LOG(WIIMOTE, "Unable to open input socket from Wiimote: %s", strerror(errno));
    close(m_int_sock);
    close(m_cmd_sock);
    m_int_sock = m_cmd_sock = -1;
    return false;
  }

  return true;
}

void WiimoteLinux::DisconnectInternal()
{
  close(m_cmd_sock);
  close(m_int_sock);

  m_cmd_sock = -1;
  m_int_sock = -1;
  char bdaddr_str[18] = {};
  ba2str(&m_bdaddr, bdaddr_str);
  s_known_addrs.erase(std::remove(s_known_addrs.begin(), s_known_addrs.end(), bdaddr_str),
                      s_known_addrs.end());
}

bool WiimoteLinux::IsConnected() const
{
  return m_cmd_sock != -1;  // && int_sock != -1;
}

void WiimoteLinux::IOWakeup()
{
  char c = 0;
  if (write(m_wakeup_pipe_w, &c, 1) != 1)
  {
    ERROR_LOG(WIIMOTE, "Unable to write to wakeup pipe.");
  }
}

// positive = read packet
// negative = didn't read packet
// zero = error
int WiimoteLinux::IORead(u8* buf)
{
  // Block select for 1/2000th of a second

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(m_int_sock, &fds);
  FD_SET(m_wakeup_pipe_r, &fds);

  if (select(std::max(m_int_sock, m_wakeup_pipe_r) + 1, &fds, nullptr, nullptr, nullptr) == -1)
  {
    ERROR_LOG(WIIMOTE, "Unable to select Wiimote %i input socket.", m_index + 1);
    return -1;
  }

  if (FD_ISSET(m_wakeup_pipe_r, &fds))
  {
    char c;
    if (read(m_wakeup_pipe_r, &c, 1) != 1)
    {
      ERROR_LOG(WIIMOTE, "Unable to read from wakeup pipe.");
    }
    return -1;
  }

  if (!FD_ISSET(m_int_sock, &fds))
    return -1;

  // Read the pending message into the buffer
  int r = read(m_int_sock, buf, MAX_PAYLOAD);
  if (r == -1)
  {
    // Error reading data
    ERROR_LOG(WIIMOTE, "Receiving data from Wiimote %i.", m_index + 1);

    if (errno == ENOTCONN)
    {
      // This can happen if the Bluetooth dongle is disconnected
      ERROR_LOG(WIIMOTE, "Bluetooth appears to be disconnected.  "
                         "Wiimote %i will be disconnected.",
                m_index + 1);
    }

    r = 0;
  }

  return r;
}

int WiimoteLinux::IOWrite(u8 const* buf, size_t len)
{
  return write(m_int_sock, buf, (int)len);
}

};  // WiimoteReal
