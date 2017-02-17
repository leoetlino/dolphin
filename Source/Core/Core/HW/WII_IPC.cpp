// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IPC.h"
#include "Core/PowerPC/PowerPC.h"

// This is the intercommunication between ARM and PPC. Currently only PPC actually uses it, because
// of the IOS HLE
// How IOS uses IPC:
// X1 Execute command: a new pointer is available in HW_IPC_PPCCTRL
// X2 Reload (a new IOS is being loaded, old one doesn't need to reply anymore)
// Y1 Command executed and reply available in HW_IPC_ARMMSG
// Y2 Command acknowledge
// ppc_msg is a pointer to 0x40byte command structure
// arm_msg is, similarly, starlet's response buffer*

namespace IOS
{
enum
{
  IPC_PPCMSG = 0x00,
  IPC_PPCCTRL = 0x04,
  IPC_ARMMSG = 0x08,
  IPC_ARMCTRL = 0x0c,

  PPCSPEED = 0x18,
  VISOLID = 0x24,

  PPC_IRQFLAG = 0x30,
  PPC_IRQMASK = 0x34,
  ARM_IRQFLAG = 0x38,
  ARM_IRQMASK = 0x3c,

  GPIOB_OUT = 0xc0,
  GPIOB_DIR = 0xc4,
  GPIOB_IN = 0xc8,
  GPIOB_INTFLAG = 0xd0,
  GPIO_OUT = 0xe0,
  GPIO_DIR = 0xe4,
  GPIO_IN = 0xe8,
  GPIO_INTFLAG = 0xf0,

  UNK_180 = 0x180,
  UNK_1CC = 0x1cc,
  UNK_1D0 = 0x1d0,
};

struct CtrlRegister
{
  u8 X1 : 1;
  u8 X2 : 1;
  u8 Y1 : 1;
  u8 Y2 : 1;
  u8 IX1 : 1;
  u8 IX2 : 1;
  u8 IY1 : 1;
  u8 IY2 : 1;

  CtrlRegister() { X1 = X2 = Y1 = Y2 = IX1 = IX2 = IY1 = IY2 = 0; }
  inline u8 ppc() { return (IY2 << 5) | (IY1 << 4) | (X2 << 3) | (Y1 << 2) | (Y2 << 1) | X1; }
  inline u8 arm() { return (IX2 << 5) | (IX1 << 4) | (Y2 << 3) | (X1 << 2) | (X2 << 1) | Y1; }
  inline void ppc(u32 v)
  {
    X1 = v & 1;
    X2 = (v >> 3) & 1;
    if ((v >> 2) & 1)
      Y1 = 0;
    if ((v >> 1) & 1)
      Y2 = 0;
    IY1 = (v >> 4) & 1;
    IY2 = (v >> 5) & 1;
  }

  inline void arm(u32 v)
  {
    Y1 = v & 1;
    Y2 = (v >> 3) & 1;
    if ((v >> 2) & 1)
      X1 = 0;
    if ((v >> 1) & 1)
      X2 = 0;
    IX1 = (v >> 4) & 1;
    IX2 = (v >> 5) & 1;
  }
};

// STATE_TO_SAVE
static u32 ppc_msg;
static u32 arm_msg;
static CtrlRegister ctrl;

static u32 ppc_irq_flags;
static u32 ppc_irq_masks;
static u32 arm_irq_flags;
static u32 arm_irq_masks;

static u32 sensorbar_power;  // do we need to care about this?

static CoreTiming::EventType* updateInterrupts;
static CoreTiming::EventType* pollSocket;
static void UpdateInterrupts(u64 = 0, s64 cyclesLate = 0);

static int socket_fd;

void DoState(PointerWrap& p)
{
  p.Do(ppc_msg);
  p.Do(arm_msg);
  p.DoPOD(ctrl);
  p.Do(ppc_irq_flags);
  p.Do(ppc_irq_masks);
  p.Do(arm_irq_flags);
  p.Do(arm_irq_masks);
  p.Do(sensorbar_power);
}

static void InitState()
{
  ctrl = CtrlRegister();
  ppc_msg = 0;
  arm_msg = 0;

  ppc_irq_flags = 0;
  ppc_irq_masks = 0;
  arm_irq_flags = 0;
  arm_irq_masks = 0;

  sensorbar_power = 0;

  ppc_irq_masks |= INT_CAUSE_IPC_BROADWAY;
}

static const int IPC_RATE = 600;

static int MsgPending(int s)
{
  int res;
  struct timeval tv;
  fd_set readfds;
  fd_set excfds;

  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&readfds);
  FD_SET(s, &readfds);
  FD_ZERO(&excfds);
  FD_SET(s, &excfds);

  res = select(s + 1, &readfds, NULL, &excfds, &tv);
  if (res == 0)
    return 0;
  if (res == -1)
    return -1;

  if (FD_ISSET(s, &excfds))
    return -1;
  if (FD_ISSET(s, &readfds))
    return 1;
  return -1;
}

static int SendAll(int s, void* buf, int len)
{
  int total = 0;
  int bytesleft = len;
  int n = 0;

  while (total < len)
  {
    n = send(s, ((char*)buf) + total, bytesleft, 0);
    if (n == -1)
      break;
    total += n;
    bytesleft -= n;
  }

  return n == -1 ? -1 : 0;
}

static int RecvAll(int s, void* buf, int len)
{
  int total = 0;
  int bytesleft = len;
  int n = 0;

  while (total < len)
  {
    n = recv(s, ((char*)buf) + total, bytesleft, 0);
    if (n == -1)
      break;
    if (n == 0)
    {
      n = -1;
      break;
    }
    total += n;
    bytesleft -= n;
  }

  return n == -1 ? -1 : 0;
}

static void PollSocket(u64 userdata, s64 cyclesLate)
{
  while (MsgPending(socket_fd) == 1)
  {
    u32 msg[2];
    if (RecvAll(socket_fd, msg, 8) < 0)
    {
      ERROR_LOG(WII_IPC, "Recv failed");
      return;
    }
    switch (msg[0])
    {
    case 8:
      ERROR_LOG(WII_IPC, "MSG: %08x %08x", msg[0], msg[1]);
      arm_msg = msg[1];
      break;
    case 12:
      ctrl.arm(msg[1]);
      INFO_LOG(WII_IPC, "ARMCTRL: %08x | %08x [Y1:%i Y2:%i X1:%i X2:%i]", arm_msg, msg[1], ctrl.Y1,
               ctrl.Y2, ctrl.X1, ctrl.X2);
      if (ctrl.Y1)
      {
        NOTICE_LOG(WII_IPC, "fd = %u, ret = %d", Memory::Read_U32(arm_msg),
                   static_cast<s32>(Memory::Read_U32(arm_msg + 4)));
      }
      CoreTiming::ScheduleEvent(0, updateInterrupts, 0);
      break;
    case 0x10000:
      ERROR_LOG(WII_IPC, "PPC state: %d", msg[1]);
      if (msg[1])
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
  CoreTiming::ScheduleEvent(SystemTimers::GetTicksPerSecond() / IPC_RATE - cyclesLate, pollSocket);
}

#define SOCK_PATH "/tmp/dolphin_ipc"

static void InitSocket()
{
  static struct sockaddr_un local, remote;
  unsigned int t;
  int len;

  int asock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (asock < 0)
  {
    ERROR_LOG(WII_IPC, "Could not open socket");
    return;
  }
  local.sun_family = AF_UNIX;
  strcpy(local.sun_path, SOCK_PATH);
  unlink(local.sun_path);
  len = strlen(local.sun_path) + sizeof(local.sun_family);

  if (bind(asock, (struct sockaddr*)&local, len) == -1)
  {
    ERROR_LOG(WII_IPC, "Could not bind socket");
    return;
  }
  if (listen(asock, 1) == -1)
  {
    ERROR_LOG(WII_IPC, "Could not listen on socket");
    return;
  }
  ERROR_LOG(WII_IPC, "Waiting for socket...");
  t = sizeof(remote);
  if ((socket_fd = accept(asock, (struct sockaddr*)&remote, &t)) == -1)
  {
    ERROR_LOG(WII_IPC, "Could not accept connection on socket");
    return;
  }
  pollSocket = CoreTiming::RegisterEvent("IPCSocket", PollSocket);
  CoreTiming::ScheduleEvent(0, pollSocket, 0);
}

void Init()
{
  InitState();
  updateInterrupts = CoreTiming::RegisterEvent("IPCInterrupt", UpdateInterrupts);
  InitSocket();
}

void Reset()
{
  INFO_LOG(WII_IPC, "Resetting ...");
  InitState();
  HLE::Reset();
}

void Shutdown()
{
  CoreTiming::RemoveEvent(pollSocket);
  close(socket_fd);
  socket_fd = -1;
}

void PPCCtrlHandler(u32, u32 val)
{
  u32 msg[2] = {4, val};
  SendAll(socket_fd, msg, 8);
  ctrl.ppc(val);

  INFO_LOG(WII_IPC, "PPCCTRL: %08x | %08x [Y1:%i Y2:%i X1:%i X2:%i]", ppc_msg, msg[1], ctrl.Y1,
           ctrl.Y2, ctrl.X1, ctrl.X2);
  if (ctrl.X1)
  {
    INFO_LOG(WII_IPC, "\033[22;34m\n%s\033[0m", HexDump(Memory::GetPointer(ppc_msg), 0x40).c_str());

    const HLE::Request request{ppc_msg};
    switch (request.command)
    {
    case HLE::IPC_CMD_OPEN:
    {
      const HLE::OpenRequest open{ppc_msg};
      WARN_LOG(WII_IPC, "open(name=%s, mode=%u)", open.path.c_str(), open.flags);
      break;
    }
    case HLE::IPC_CMD_CLOSE:
    {
      WARN_LOG(WII_IPC, "close(fd=%u)", request.fd);
      break;
    }
    case HLE::IPC_CMD_READ:
    case HLE::IPC_CMD_WRITE:
    {
      const HLE::ReadWriteRequest rw{ppc_msg};
      WARN_LOG(WII_IPC, "%s(fd=%u, buffer=%08x, size=%u)",
               request.command == HLE::IPC_CMD_READ ? "read" : "write", request.fd, rw.buffer,
               rw.size);
      break;
    }
    case HLE::IPC_CMD_SEEK:
    {
      const HLE::SeekRequest seek{ppc_msg};
      WARN_LOG(WII_IPC, "seek(fd=%u, whence=%u, where=%u)", request.fd, seek.mode, seek.offset);
      break;
    }
    case HLE::IPC_CMD_IOCTL:
    {
      const HLE::IOCtlRequest ioctl{ppc_msg};
      WARN_LOG(WII_IPC, "ioctl(fd=%u, request=%x, in=%08x, in_size=%u, out=%08x, out_size=%u)",
               request.fd, ioctl.request, ioctl.buffer_in, ioctl.buffer_in_size, ioctl.buffer_out,
               ioctl.buffer_out_size);
      break;
    }
    case HLE::IPC_CMD_IOCTLV:
    {
      const HLE::IOCtlVRequest ioctlv{ppc_msg};
      WARN_LOG(WII_IPC, "ioctlv(fd=%u, request=%x, in_count=%zu, out_count=%zu)", request.fd,
               ioctlv.request, ioctlv.in_vectors.size(), ioctlv.io_vectors.size());
      break;
    }
    default:
      ERROR_LOG(WII_IPC, "Unknown IPC command");
    }
    // HLE::EnqueueRequest(ppc_msg);
  }
  // HLE::Update();
  CoreTiming::ScheduleEvent(0, updateInterrupts, 0);
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  mmio->Register(base | IPC_PPCMSG, MMIO::ComplexRead<u32>([](u32) { return ppc_msg; }),
                 MMIO::ComplexWrite<u32>([](u32, u32 val) {
                   u32 msg[2] = {0, val};
                   SendAll(socket_fd, msg, 8);
                   ppc_msg = val;
                   INFO_LOG(WII_IPC, "PPCMSG: %08x", ppc_msg);
                 }));

  mmio->Register(base | IPC_PPCCTRL, MMIO::ComplexRead<u32>([](u32) { return ctrl.ppc(); }),
                 MMIO::ComplexWrite<u32>(PPCCtrlHandler));

  mmio->Register(base | IPC_ARMMSG, MMIO::DirectRead<u32>(&arm_msg), MMIO::InvalidWrite<u32>());

  mmio->Register(base | PPC_IRQFLAG, MMIO::ComplexRead<u32>([](u32) { return ppc_irq_flags; }),
                 MMIO::ComplexWrite<u32>([](u32, u32 val) {
                   ppc_irq_flags &= ~val;
                   HLE::Update();
                   CoreTiming::ScheduleEvent(0, updateInterrupts, 0);
                 }));

  mmio->Register(base | PPC_IRQMASK, MMIO::ComplexRead<u32>([](u32) { return ppc_irq_masks; }),
                 MMIO::ComplexWrite<u32>([](u32, u32 val) {
                   ppc_irq_masks = val;
                   // if (ppc_irq_masks & INT_CAUSE_IPC_BROADWAY)  // wtf?
                   //   Reset();
                   // HLE::Update();
                   CoreTiming::ScheduleEvent(0, updateInterrupts, 0);
                 }));

  mmio->Register(base | GPIOB_OUT, MMIO::Constant<u32>(0),
                 MMIO::DirectWrite<u32>(&sensorbar_power));

  // Register some stubbed/unknown MMIOs required to make Wii games work.
  mmio->Register(base | PPCSPEED, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  mmio->Register(base | VISOLID, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  mmio->Register(base | GPIOB_DIR, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | GPIOB_IN, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | GPIOB_INTFLAG, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | GPIO_DIR, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | GPIO_IN, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | GPIO_INTFLAG, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | UNK_180, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | UNK_1CC, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
  mmio->Register(base | UNK_1D0, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
}

static void UpdateInterrupts(u64 userdata, s64 cyclesLate)
{
  if ((ctrl.Y1 & ctrl.IY1) || (ctrl.Y2 & ctrl.IY2))
  {
    printf("INT_CAUSE_IPC_BROADWAY\n");
    ppc_irq_flags |= INT_CAUSE_IPC_BROADWAY;
  }

  if ((ctrl.X1 & ctrl.IX1) || (ctrl.X2 & ctrl.IX2))
  {
    printf("INT_CAUSE_IPC_STARLET\n");
    ppc_irq_flags |= INT_CAUSE_IPC_STARLET;
  }

  // Generate interrupt on PI if any of the devices behind starlet have an interrupt and mask is set
  ProcessorInterface::SetInterrupt(ProcessorInterface::INT_CAUSE_WII_IPC,
                                   !!(ppc_irq_flags & ppc_irq_masks));
}

void GenerateAck(u32 _Address)
{
  arm_msg = _Address;  // dunno if it's really set here, but HLE needs to stay in context
  ctrl.Y2 = 1;
  DEBUG_LOG(WII_IPC, "GenerateAck: %08x | %08x [R:%i A:%i E:%i]", ppc_msg, _Address, ctrl.Y1,
            ctrl.Y2, ctrl.X1);
  CoreTiming::ScheduleEvent(1000, updateInterrupts, 0);
}

void GenerateReply(u32 _Address)
{
  arm_msg = _Address;
  ctrl.Y1 = 1;
  DEBUG_LOG(WII_IPC, "GenerateReply: %08x | %08x [R:%i A:%i E:%i]", ppc_msg, _Address, ctrl.Y1,
            ctrl.Y2, ctrl.X1);
  UpdateInterrupts();
}

bool IsReady()
{
  return ((ctrl.Y1 == 0) && (ctrl.Y2 == 0) && ((ppc_irq_flags & INT_CAUSE_IPC_BROADWAY) == 0));
}
}  // namespace IOS
