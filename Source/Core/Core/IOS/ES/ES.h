// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOS.h"

class PointerWrap;

namespace DiscIO
{
class CNANDContentLoader;
}

namespace IOS
{
namespace HLE
{
namespace Device
{
struct TitleContext
{
  void Clear();
  void DoState(PointerWrap& p);
  void Update(const DiscIO::CNANDContentLoader& content_loader);
  void Update(const IOS::ES::TMDReader& tmd_, const IOS::ES::TicketReader& ticket_);

  IOS::ES::TicketReader ticket;
  IOS::ES::TMDReader tmd;
  bool active = false;
  bool first_change = true;
};

class ES final : public Device
{
public:
  ES(Kernel& ios, const std::string& device_name);

  static void LoadWAD(const std::string& _rContentFile);

  void DoState(PointerWrap& p) override;

  ReturnCode Open(const OpenRequest& request) override;
  ReturnCode Close(u32 fd) override;
  IPCCommandResult IOCtlV(const IOCtlVRequest& request) override;

  struct OpenedContent
  {
    u64 m_title_id;
    IOS::ES::Content m_content;
    u32 m_position;
  };

  struct TitleImportContext
  {
    IOS::ES::TMDReader tmd;
    u32 content_id = 0xFFFFFFFF;
    std::vector<u8> content_buffer;
  };

  // TODO: merge this with TitleImportContext. Also reuse the global content table.
  struct TitleExportContext
  {
    struct ExportContent
    {
      OpenedContent content;
      std::array<u8, 16> iv{};
    };

    bool valid = false;
    IOS::ES::TMDReader tmd;
    std::vector<u8> title_key;
    std::map<u32, ExportContent> contents;
  };

  struct Context
  {
    void DoState(PointerWrap& p);

    u16 gid = 0;
    u32 uid = 0;
    TitleImportContext title_import;
    TitleExportContext title_export;
    bool active = false;
    // We use this to associate an IPC fd with an ES context.
    u32 ipc_fd = -1;
  };

  // Title management
  ReturnCode ImportTicket(const std::vector<u8>& ticket_bytes);
  ReturnCode ImportTmd(Context& context, const std::vector<u8>& tmd_bytes);
  ReturnCode ImportTitleInit(Context& context, const std::vector<u8>& tmd_bytes);
  ReturnCode ImportContentBegin(Context& context, u64 title_id, u32 content_id);
  ReturnCode ImportContentData(Context& context, u32 content_fd, const u8* data, u32 data_size);
  ReturnCode ImportContentEnd(Context& context, u32 content_fd);
  ReturnCode ImportTitleDone(Context& context);
  ReturnCode ImportTitleCancel(Context& context);
  ReturnCode ExportTitleInit(Context& context, u64 title_id, u8* tmd, u32 tmd_size);
  ReturnCode ExportContentBegin(Context& context, u64 title_id, u32 content_id);
  ReturnCode ExportContentData(Context& context, u32 content_fd, u8* data, u32 data_size);
  ReturnCode ExportContentEnd(Context& context, u32 content_fd);
  ReturnCode ExportTitleDone(Context& context);
  ReturnCode DeleteTitle(u64 title_id);
  ReturnCode DeleteTitleContent(u64 title_id) const;
  ReturnCode DeleteTicket(const u8* ticket_view);

  // Device identity and encryption
  ReturnCode GetDeviceId(u32* device_id);
  ReturnCode GetDeviceCert(u8* cert);
  ReturnCode CheckKoreaRegion();
  ReturnCode Sign(const u8* data, u32 data_size, u8* signature, u8* ecc_certificate);
  ReturnCode Encrypt(u32 uid, u32 encrypt_handle, u8* iv, const u8* input, u32 size, u8* output);
  ReturnCode Decrypt(u32 uid, u32 decrypt_handle, u8* iv, const u8* input, u32 size, u8* output);

  // Misc
  ReturnCode SetUid(u32 uid, u64 title_id);
  ReturnCode GetDataDir(u64 title_id, std::string* data_directory) const;
  ReturnCode GetTitleId(u64* title_id) const;
  ReturnCode GetConsumption();
  ReturnCode GetBoot2Version(u32* version);
  ReturnCode LaunchTitle(u64 title_id, bool skip_reload = false);
  ReturnCode LaunchBC();
  ReturnCode DiVerify(const IOS::ES::TMDReader& tmd, const IOS::ES::TicketReader& ticket);

  // Title contents
  ReturnCode OpenTitleContentFile(u32 uid, u64 title_id, const u8* ticket_view, u16 cidx);
  ReturnCode OpenContentFile(u32 uid, u16 cidx);
  ReturnCode ReadContentFile(u32 uid, s32 cfd, u8* data, u32 data_size);
  ReturnCode CloseContentFile(u32 uid, s32 cfd);
  ReturnCode SeekContentFile(u32 uid, s32 cfd, u32 where, u32 whence);

  // Title information
  ReturnCode ListOwnedTitles(std::vector<u64>* titles);
  ReturnCode ListTitles(std::vector<u64>* titles);
  ReturnCode ListTitleContents(u64 title_id, std::vector<u32>* contents);
  ReturnCode ListTmdContents(const IOS::ES::TMDReader& tmd, std::vector<u32>* contents);
  ReturnCode ListSharedContents(std::vector<std::array<u8, 20>>* contents) const;
  ReturnCode GetTmd(u64 title_id, IOS::ES::TMDReader* tmd);
  ReturnCode DIGetTmd(IOS::ES::TMDReader* tmd);

  // Views for tickets and TMDs
  ReturnCode GetTicketViews(const IOS::ES::TicketReader& ticket, u8* ticket_views);
  ReturnCode GetTicketViews(u64 title_id, u8* views, u32* view_count);
  ReturnCode GetTmdView(const IOS::ES::TMDReader& tmd, u8* view, u32* view_size);
  ReturnCode GetTmdView(u64 title_id, u8* view, u32* view_size);

private:
  enum
  {
    IOCTL_ES_ADDTICKET = 0x01,
    IOCTL_ES_ADDTITLESTART = 0x02,
    IOCTL_ES_ADDCONTENTSTART = 0x03,
    IOCTL_ES_ADDCONTENTDATA = 0x04,
    IOCTL_ES_ADDCONTENTFINISH = 0x05,
    IOCTL_ES_ADDTITLEFINISH = 0x06,
    IOCTL_ES_GETDEVICEID = 0x07,
    IOCTL_ES_LAUNCH = 0x08,
    IOCTL_ES_OPENCONTENT = 0x09,
    IOCTL_ES_READCONTENT = 0x0A,
    IOCTL_ES_CLOSECONTENT = 0x0B,
    IOCTL_ES_GETOWNEDTITLECNT = 0x0C,
    IOCTL_ES_GETOWNEDTITLES = 0x0D,
    IOCTL_ES_GETTITLECNT = 0x0E,
    IOCTL_ES_GETTITLES = 0x0F,
    IOCTL_ES_GETTITLECONTENTSCNT = 0x10,
    IOCTL_ES_GETTITLECONTENTS = 0x11,
    IOCTL_ES_GETVIEWCNT = 0x12,
    IOCTL_ES_GETVIEWS = 0x13,
    IOCTL_ES_GETTMDVIEWCNT = 0x14,
    IOCTL_ES_GETTMDVIEWS = 0x15,
    IOCTL_ES_GETCONSUMPTION = 0x16,
    IOCTL_ES_DELETETITLE = 0x17,
    IOCTL_ES_DELETETICKET = 0x18,
    IOCTL_ES_DIGETTMDVIEWSIZE = 0x19,
    IOCTL_ES_DIGETTMDVIEW = 0x1A,
    IOCTL_ES_DIGETTICKETVIEW = 0x1B,
    IOCTL_ES_DIVERIFY = 0x1C,
    IOCTL_ES_GETTITLEDIR = 0x1D,
    IOCTL_ES_GETDEVICECERT = 0x1E,
    IOCTL_ES_IMPORTBOOT = 0x1F,
    IOCTL_ES_GETTITLEID = 0x20,
    IOCTL_ES_SETUID = 0x21,
    IOCTL_ES_DELETETITLECONTENT = 0x22,
    IOCTL_ES_SEEKCONTENT = 0x23,
    IOCTL_ES_OPENTITLECONTENT = 0x24,
    IOCTL_ES_LAUNCHBC = 0x25,
    IOCTL_ES_EXPORTTITLEINIT = 0x26,
    IOCTL_ES_EXPORTCONTENTBEGIN = 0x27,
    IOCTL_ES_EXPORTCONTENTDATA = 0x28,
    IOCTL_ES_EXPORTCONTENTEND = 0x29,
    IOCTL_ES_EXPORTTITLEDONE = 0x2A,
    IOCTL_ES_ADDTMD = 0x2B,
    IOCTL_ES_ENCRYPT = 0x2C,
    IOCTL_ES_DECRYPT = 0x2D,
    IOCTL_ES_GETBOOT2VERSION = 0x2E,
    IOCTL_ES_ADDTITLECANCEL = 0x2F,
    IOCTL_ES_SIGN = 0x30,
    IOCTL_ES_VERIFYSIGN = 0x31,
    IOCTL_ES_GETSTOREDCONTENTCNT = 0x32,
    IOCTL_ES_GETSTOREDCONTENTS = 0x33,
    IOCTL_ES_GETSTOREDTMDSIZE = 0x34,
    IOCTL_ES_GETSTOREDTMD = 0x35,
    IOCTL_ES_GETSHAREDCONTENTCNT = 0x36,
    IOCTL_ES_GETSHAREDCONTENTS = 0x37,
    IOCTL_ES_DELETESHAREDCONTENT = 0x38,
    IOCTL_ES_DIGETTMDSIZE = 0x39,
    IOCTL_ES_DIGETTMD = 0x3A,
    IOCTL_ES_UNKNOWN_3B = 0x3B,
    IOCTL_ES_UNKNOWN_3C = 0x3C,
    IOCTL_ES_UNKNOWN_3D = 0x3D,
    IOCTL_ES_UNKNOWN_3E = 0x3E,
    IOCTL_ES_UNKNOWN_3F = 0x3F,
    IOCTL_ES_UNKNOWN_40 = 0x40,
    IOCTL_ES_UNKNOWN_41 = 0x41,
    IOCTL_ES_UNKNOWN_42 = 0x42,
    IOCTL_ES_UNKNOWN_43 = 0x43,
    IOCTL_ES_UNKNOWN_44 = 0x44,
    IOCTL_ES_CHECKKOREAREGION = 0x45,
  };

  // Title management
  IPCCommandResult ImportTicket(const IOCtlVRequest& request);
  IPCCommandResult ImportTmd(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportTitleInit(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportContentBegin(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportContentData(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportContentEnd(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportTitleDone(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ImportTitleCancel(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ExportTitleInit(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ExportContentBegin(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ExportContentData(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ExportContentEnd(Context& context, const IOCtlVRequest& request);
  IPCCommandResult ExportTitleDone(Context& context, const IOCtlVRequest& request);
  IPCCommandResult DeleteTitle(const IOCtlVRequest& request);
  IPCCommandResult DeleteTitleContent(const IOCtlVRequest& request);
  IPCCommandResult DeleteTicket(const IOCtlVRequest& request);

  // Device identity and encryption
  IPCCommandResult GetDeviceId(const IOCtlVRequest& request);
  IPCCommandResult GetDeviceCert(const IOCtlVRequest& request);
  IPCCommandResult CheckKoreaRegion(const IOCtlVRequest& request);
  IPCCommandResult Sign(const IOCtlVRequest& request);
  IPCCommandResult Encrypt(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult Decrypt(u32 uid, const IOCtlVRequest& request);

  // Misc
  IPCCommandResult SetUid(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult GetDataDir(const IOCtlVRequest& request);
  IPCCommandResult GetTitleId(const IOCtlVRequest& request);
  IPCCommandResult GetBoot2Version(const IOCtlVRequest& request);
  IPCCommandResult GetConsumption(const IOCtlVRequest& request);
  IPCCommandResult LaunchTitle(const IOCtlVRequest& request);
  IPCCommandResult LaunchBC(const IOCtlVRequest& request);
  IPCCommandResult DiVerify(const IOCtlVRequest& request);

  // Title contents
  IPCCommandResult OpenTitleContentFile(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult OpenContentFile(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult ReadContentFile(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult CloseContentFile(u32 uid, const IOCtlVRequest& request);
  IPCCommandResult SeekContentFile(u32 uid, const IOCtlVRequest& request);

  // Title information
  IPCCommandResult ListOwnedTitlesCount(const IOCtlVRequest& request);
  IPCCommandResult ListOwnedTitles(const IOCtlVRequest& request);
  IPCCommandResult ListTitlesCount(const IOCtlVRequest& request);
  IPCCommandResult ListTitles(const IOCtlVRequest& request);
  IPCCommandResult ListTitleContentsCount(const IOCtlVRequest& request);
  IPCCommandResult ListTitleContents(const IOCtlVRequest& request);
  IPCCommandResult ListTmdContentsCount(const IOCtlVRequest& request);
  IPCCommandResult ListTmdContents(const IOCtlVRequest& request);
  IPCCommandResult ListSharedContentsCount(const IOCtlVRequest& request) const;
  IPCCommandResult ListSharedContents(const IOCtlVRequest& request) const;
  IPCCommandResult GetTmdSize(const IOCtlVRequest& request);
  IPCCommandResult GetTmd(const IOCtlVRequest& request);
  IPCCommandResult DiGetTmdSize(const IOCtlVRequest& request);
  IPCCommandResult DiGetTmd(const IOCtlVRequest& request);

  // Views for tickets and TMDs
  IPCCommandResult GetTicketViewsCount(const IOCtlVRequest& request);
  IPCCommandResult GetTicketViews(const IOCtlVRequest& request);
  IPCCommandResult GetTmdViewSize(const IOCtlVRequest& request);
  IPCCommandResult GetTmdView(const IOCtlVRequest& request);
  IPCCommandResult GetTmdViewSizeFromTitleId(const IOCtlVRequest& request);
  IPCCommandResult GetTmdViewFromTitleId(const IOCtlVRequest& request);
  IPCCommandResult DiGetTicketView(const IOCtlVRequest& request);

  // ES can only have 3 contexts at one time.
  using ContextArray = std::array<Context, 3>;

  ContextArray::iterator FindActiveContext(u32 fd);
  ContextArray::iterator FindInactiveContext();

  ReturnCode LaunchIOS(u64 ios_title_id);
  ReturnCode LaunchPPCTitle(u64 title_id, bool skip_reload);
  static TitleContext& GetTitleContext();

  static const DiscIO::CNANDContentLoader& AccessContentDevice(u64 title_id);

  u32 OpenTitleContent(u32 CFD, u64 TitleID, u16 Index);

  using ContentAccessMap = std::map<u32, OpenedContent>;
  ContentAccessMap m_ContentAccessMap;

  u32 m_AccessIdentID = 0;

  ContextArray m_contexts;
};
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
