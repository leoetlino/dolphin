// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/DeviceLLE.h"
#include "Core/PowerPC/PowerPC.h"

namespace IOS
{
class SkyeyeIPC final
{
public:
  SkyeyeIPC();
  ~SkyeyeIPC();

  void SendRequest(u32 ppc_msg, std::function<void()> callback = {});
  void SendAck();
  void SetOnIOSAckCallback(std::function<void()> callback) { m_on_ack_callback = callback; }
  bool IsPPCAlive() const { return m_ppc_alive; }
private:
  void InitSocket();

  int m_socket_fd = -1;
  int m_wakeup_eventfd = -1;
  std::thread m_ipc_thread;
  Common::Flag m_ipc_thread_running;

  bool m_ppc_alive = false;

  std::mutex m_requests_mutex;
  std::map<u32, std::function<void()>> m_requests;
  std::function<void()> m_on_ack_callback;
};

SkyeyeIPC::SkyeyeIPC()
{
  InitSocket();
  if (m_socket_fd < 0)
  {
    ERROR_LOG(IOS, "Failed to init socket");
    return;
  }
  m_wakeup_eventfd = eventfd(0, 0);
  m_ipc_thread_running.Set();
  m_ipc_thread = std::thread([this]() {
    Common::SetCurrentThreadName("Skyeye IPC");
    NOTICE_LOG(IOS, "Skyeye IPC thread started");
    while (m_ipc_thread_running.IsSet())
    {
      fd_set fds;

      FD_ZERO(&fds);
      FD_SET(m_socket_fd, &fds);
      FD_SET(m_wakeup_eventfd, &fds);

      int ret = select(m_wakeup_eventfd + 1, &fds, nullptr, nullptr, nullptr);
      if (ret < 1 || !FD_ISSET(m_socket_fd, &fds))
        continue;

      u32 msg[3];
      if (read(m_socket_fd, &msg[0], 4) != 4 || read(m_socket_fd, &msg[1], 4) != 4 ||
          read(m_socket_fd, &msg[2], 4) != 4)
      {
        ERROR_LOG(WII_IPC, "read failed: %s", strerror(errno));
        continue;
      }

      INFO_LOG(IOS, "MSG %08x  %08x", msg[0], msg[1]);
      switch (msg[0])
      {
      case 1:  // MSG_MESSAGE
      {
        std::lock_guard<std::mutex> lock{m_requests_mutex};
        const auto iterator = m_requests.find(msg[1]);
        if (iterator == m_requests.end())
        {
          ERROR_LOG(IOS, "Unhandled IPC request: %08x", msg[1]);
          continue;
        }
        INFO_LOG(IOS, "\033[22;34mIOS replied to IPC request: %08x\n%s\033[0m", msg[1],
                 HexDump(Memory::GetPointer(msg[1]), 0x40).c_str());
        iterator->second();
        m_requests.erase(iterator);
        break;
      }
      case 2:
      {
        WARN_LOG(IOS, "IOS ack");
        SendAck();
        if (m_on_ack_callback)
          m_on_ack_callback();
        break;
      }
      case 3:  // MSG_STATUS
      {
        m_ppc_alive = msg[1] == 1;
        ERROR_LOG(WII_IPC, "PPC state: %d", msg[1]);
        if (m_ppc_alive)
        {
          MSR = 0;
          PC = 0x3400;
        }
        else
        {
          // Put it into an infinite loop for now...
          MSR = 0;
          PC = 0;
          Memory::Write_U32(0x48000000, 0x00000000);
        }
        break;
      }
      }
    }
    NOTICE_LOG(IOS, "Skyeye IPC thread stopped");
  });
}

SkyeyeIPC::~SkyeyeIPC()
{
  m_ipc_thread_running.Clear();
  // Write something to efd so that select() stops blocking.
  u64 value = 1;
  if (write(m_wakeup_eventfd, &value, sizeof(u64)) < 0)
  {
    ERROR_LOG(IOS, "Failed to wake up IPC thread");
  }
  m_ipc_thread.join();
  close(m_wakeup_eventfd);
  close(m_socket_fd);
}

void SkyeyeIPC::SendRequest(u32 ppc_msg, std::function<void()> callback)
{
  if (m_socket_fd < 0)
  {
    ERROR_LOG(IOS, "Invalid socket");
    return;
  }

  std::lock_guard<std::mutex> lk{m_requests_mutex};
  m_requests.emplace(ppc_msg, callback);
  u32 msg[3] = {1, ppc_msg, 0};
  if (write(m_socket_fd, msg, 12) < 0)
    ERROR_LOG(IOS, "Failed to send message");
}

void SkyeyeIPC::SendAck()
{
  if (m_socket_fd < 0)
  {
    ERROR_LOG(IOS, "Invalid socket");
    return;
  }

  u32 msg[3] = {2, 0, 0};
  write(m_socket_fd, msg, 12);
}

void SkyeyeIPC::InitSocket()
{
  m_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_socket_fd < 0)
  {
    ERROR_LOG(WII_IPC, "Could not open socket");
    m_socket_fd = -1;
    return;
  }

  static sockaddr_un remote;
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, "/tmp/dolphin_ipc");
  int len = strlen(remote.sun_path) + sizeof(remote.sun_family);
  if (connect(m_socket_fd, reinterpret_cast<sockaddr*>(&remote), len) == -1)
  {
    ERROR_LOG(IOS, "Failed to connect to Skyeye");
    m_socket_fd = -1;
    return;
  }
  NOTICE_LOG(IOS, "Connected to Skyeye");
}

static std::unique_ptr<SkyeyeIPC> s_skyeye;

namespace LLE
{
void Init()
{
  s_skyeye = std::make_unique<SkyeyeIPC>();
  NOTICE_LOG(IOS, "Waiting for IOS to bring up the PPC");
  while (!s_skyeye->IsPPCAlive())
  {
    Common::SleepCurrentThread(50);
  }
  NOTICE_LOG(IOS, "Sending ack");
  s_skyeye->SendAck();
  NOTICE_LOG(IOS, "IOS IPC initialized");
}

void Reset()
{
  // Relaunch IOS?
}

void Shutdown()
{
  s_skyeye.reset();
}
}

namespace HLE
{
namespace Device
{
LLE::LLE(u32 device_id, const std::string& device_name) : Device(device_id, device_name)
{
}

IPCCommandResult LLE::Open(const Request& request)
{
  const u32 address = request.address;
  s_skyeye->SendRequest(request.address, [this, address]() {
    const s32 ret = Memory::Read_U32(address + 4);
    m_fd = ret;
    if (ret >= 0)
      Memory::Write_U32(m_device_id, address + 4);
    NOTICE_LOG(WII_IPC, "open(%s) = %d (IOS: %d)", m_name.c_str(), Memory::Read_U32(address + 4),
               ret);
    DirectlyEnqueueReply(address, CoreTiming::FromThread::NON_CPU);
    // Delete this device so the fd slot is freed.
    if (ret < 0)
      RemoveDevice(m_device_id);
  });
  return GetNoReply();
}

IPCCommandResult LLE::Close(const Request& request)
{
  WARN_LOG(WII_IPC, "close(fd=%u (%s))", request.fd, m_name.c_str());
  SendRequest(request.address);
  return GetNoReply();
}

IPCCommandResult LLE::Read(const ReadWriteRequest& request)
{
  WARN_LOG(WII_IPC, "%s(fd=%u (%s), buffer=%08x, size=%u)",
           request.command == HLE::IPC_CMD_READ ? "read" : "write", request.fd, m_name.c_str(),
           request.buffer, request.size);
  SendRequest(request.address);
  return GetNoReply();
}

IPCCommandResult LLE::Write(const ReadWriteRequest& request)
{
  WARN_LOG(WII_IPC, "%s(fd=%u (%s), buffer=%08x, size=%u)",
           request.command == HLE::IPC_CMD_READ ? "read" : "write", request.fd, m_name.c_str(),
           request.buffer, request.size);
  SendRequest(request.address);
  return GetNoReply();
}

IPCCommandResult LLE::Seek(const SeekRequest& request)
{
  WARN_LOG(WII_IPC, "seek(fd=%u, whence=%u, where=%u)", request.fd, request.mode, request.offset);
  SendRequest(request.address);
  return GetNoReply();
}

IPCCommandResult LLE::IOCtl(const IOCtlRequest& request)
{
  WARN_LOG(WII_IPC, "ioctl(fd=%u (%s), request=%x, in=%08x, in_size=%u, out=%08x, out_size=%u)",
           request.fd, m_name.c_str(), request.request, request.buffer_in, request.buffer_in_size,
           request.buffer_out, request.buffer_out_size);
  SendRequest(request.address);
  return GetNoReply();
}

IPCCommandResult LLE::IOCtlV(const IOCtlVRequest& request)
{
  if (m_name == "/dev/es")
  {
    if (request.request == 0x8)
    {
      if (Memory::Read_U64(request.in_vectors[0].address) == 0x000000100000009)
      {
        WARN_LOG(IOS, "Detected attempt to launch IOS9; forcing IOS11");
        Memory::Write_U64(0x00000010000000b, request.in_vectors[0].address);
      }
      Reload(Memory::Read_U64(request.in_vectors[0].address));
      SendRequest(request.address);
      s_skyeye->SetOnIOSAckCallback([&request]() {
        s_skyeye->SetOnIOSAckCallback(nullptr);
        EnqueueCommandAcknowledgement(request.address, 0);
      });
      return GetNoReply();
    }
    if (request.request == 0x12 || request.request == 0x13)
    {
      if (Memory::Read_U64(request.in_vectors[0].address) == 0x000000100000009)
      {
        WARN_LOG(IOS, "Detected attempt to get ticket views for IOS9; forcing IOS11");
        Memory::Write_U64(0x00000010000000b, request.in_vectors[0].address);
        SendRequest(request.address);
        return GetNoReply();
      }
    }
  }

  WARN_LOG(WII_IPC, "ioctlv(fd=%u (%s), request=%x, in_count=%zu, out_count=%zu)", request.fd,
           m_name.c_str(), request.request, request.in_vectors.size(), request.io_vectors.size());
  SendRequest(request.address);
  return GetNoReply();
}

void LLE::SendRequest(u32 address)
{
  Memory::Write_U32(m_fd, address + 8);
  s_skyeye->SendRequest(address, [this, address]() {
    DirectlyEnqueueReply(address, CoreTiming::FromThread::NON_CPU);
  });
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
