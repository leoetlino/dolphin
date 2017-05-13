// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/ES/ES.h"

#include <cinttypes>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/NANDContentLoader.h"

namespace IOS
{
namespace HLE
{
namespace Device
{
// TODO: drop this.
u32 ES::OpenTitleContent(u32 CFD, u64 TitleID, u16 Index)
{
  const DiscIO::CNANDContentLoader& Loader = AccessContentDevice(TitleID);

  if (!Loader.IsValid() || !Loader.GetTMD().IsValid() || !Loader.GetTicket().IsValid())
  {
    WARN_LOG(IOS_ES, "ES: loader not valid for %" PRIx64, TitleID);
    return 0xffffffff;
  }

  const DiscIO::SNANDContent* pContent = Loader.GetContentByIndex(Index);

  if (pContent == nullptr)
  {
    return 0xffffffff;  // TODO: what is the correct error value here?
  }

  OpenedContent content;
  content.m_position = 0;
  content.m_content = pContent->m_metadata;
  content.m_title_id = TitleID;

  pContent->m_Data->Open();

  m_ContentAccessMap[CFD] = content;
  INFO_LOG(IOS_ES, "OpenTitleContent: TitleID: %016" PRIx64 "  Index %i -> got CFD %x",
           TitleID, Index, CFD);
  return CFD;
}

ReturnCode ES::OpenTitleContentFile(u32 uid, u64 title_id, const u8* ticket_view, u16 cidx)
{
  return static_cast<ReturnCode>(OpenTitleContent(m_AccessIdentID++, title_id, cidx));
}

IPCCommandResult ES::OpenTitleContentFile(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return GetDefaultReply(ES_EINVAL);

  const u64 title_id = Memory::Read_U64(request.in_vectors[0].address);
  const u8* ticket_view = Memory::GetPointer(request.in_vectors[1].address);
  const u32 index = Memory::Read_U32(request.in_vectors[2].address);

  return GetDefaultReply(OpenTitleContentFile(uid, title_id, ticket_view, index));
}

ReturnCode ES::OpenContentFile(u32 uid, u16 cidx)
{
  if (!GetTitleContext().active)
    return ES_EINVAL;

  const u64 title_id = GetTitleContext().tmd.GetTitleId();
  return static_cast<ReturnCode>(OpenTitleContent(m_AccessIdentID++, title_id, cidx));
}

IPCCommandResult ES::OpenContentFile(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_EINVAL);

  const u32 index = Memory::Read_U32(request.in_vectors[0].address);
  return GetDefaultReply(OpenContentFile(uid, index));
}

ReturnCode ES::ReadContentFile(u32 uid, s32 cfd, u8 *data, u32 data_size)
{
  auto itr = m_ContentAccessMap.find(cfd);
  if (itr == m_ContentAccessMap.end())
    return ES_EINVAL;

  OpenedContent& rContent = itr->second;

  if (rContent.m_position + data_size > rContent.m_content.size)
  {
    data_size = static_cast<u32>(rContent.m_content.size) - rContent.m_position;
  }

  if (data_size > 0)
  {
    if (data)
    {
      const DiscIO::CNANDContentLoader& ContentLoader = AccessContentDevice(rContent.m_title_id);
      // ContentLoader should never be invalid; rContent has been created by it.
      if (ContentLoader.IsValid() && ContentLoader.GetTicket().IsValid())
      {
        const DiscIO::SNANDContent* pContent =
            ContentLoader.GetContentByIndex(rContent.m_content.index);
        if (!pContent->m_Data->GetRange(rContent.m_position, data_size, data))
          ERROR_LOG(IOS_ES, "ES: failed to read %u bytes from %u!", data_size, rContent.m_position);
      }

      rContent.m_position += data_size;
    }
    else
    {
      PanicAlert("IOCTL_ES_READCONTENT - bad destination");
    }
  }

  return static_cast<ReturnCode>(data_size);
}

IPCCommandResult ES::ReadContentFile(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return GetDefaultReply(ES_EINVAL);

  const s32 cfd = Memory::Read_U32(request.in_vectors[0].address);
  u8* data = Memory::GetPointer(request.io_vectors[0].address);
  const u32 data_size = request.io_vectors[0].size;

  return GetDefaultReply(ReadContentFile(uid, cfd, data, data_size));
}

ReturnCode ES::CloseContentFile(u32 uid, s32 cfd)
{
  INFO_LOG(IOS_ES, "CloseContentFile: CFD %x", cfd);

  auto itr = m_ContentAccessMap.find(cfd);
  if (itr == m_ContentAccessMap.end())
    return ES_EINVAL;

  const DiscIO::CNANDContentLoader& ContentLoader = AccessContentDevice(itr->second.m_title_id);
  // ContentLoader should never be invalid; we shouldn't be here if ES_OPENCONTENT failed before.
  if (ContentLoader.IsValid())
  {
    const DiscIO::SNANDContent* pContent =
        ContentLoader.GetContentByIndex(itr->second.m_content.index);
    pContent->m_Data->Close();
  }

  m_ContentAccessMap.erase(itr);
  return IPC_SUCCESS;
}

IPCCommandResult ES::CloseContentFile(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return GetDefaultReply(ES_EINVAL);

  const s32 cfd = Memory::Read_U32(request.in_vectors[0].address);
  return GetDefaultReply(CloseContentFile(uid, cfd));
}

ReturnCode ES::SeekContentFile(u32 uid, s32 cfd, u32 where, u32 whence)
{
  auto itr = m_ContentAccessMap.find(cfd);
  if (itr == m_ContentAccessMap.end())
    return ES_EINVAL;

  OpenedContent& rContent = itr->second;

  switch (whence)
  {
  case 0:  // SET
    rContent.m_position = where;
    break;

  case 1:  // CUR
    rContent.m_position += where;
    break;

  case 2:  // END
    rContent.m_position = static_cast<u32>(rContent.m_content.size) + where;
    break;
  }

  return static_cast<ReturnCode>(rContent.m_position);
}

IPCCommandResult ES::SeekContentFile(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return GetDefaultReply(ES_EINVAL);

  const s32 cfd = Memory::Read_U32(request.in_vectors[0].address);
  const u32 where = Memory::Read_U32(request.in_vectors[1].address);
  const u32 whence = Memory::Read_U32(request.in_vectors[2].address);

  return GetDefaultReply(SeekContentFile(uid, cfd, where, whence));
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
