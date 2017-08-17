// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <mbedtls/aes.h>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOS.h"

namespace IOS
{
namespace HLE
{
class ARCUnpacker
{
public:
  ARCUnpacker() { Reset(); }
  void Reset();

  void AddBytes(const std::vector<u8>& bytes);

  using WriteCallback = std::function<void(const std::string&, const std::vector<u8>&)>;
  void Extract(const WriteCallback& callback);

private:
  std::vector<u8> m_whole_file;
};

namespace Device
{
class WFSI : public Device
{
public:
  WFSI(Kernel& ios, const std::string& device_name);

  IPCCommandResult IOCtl(const IOCtlRequest& request) override;

private:
  u32 GetTmd(u16 group_id, u32 title_id, u64 subtitle_id, u32 address, u32* size) const;

  s32 CancelTitleImport();
  s32 CancelPatchImport();

  struct TitleId
  {
    void Set(u64 value);
    const char* c_str() const { return string.c_str(); }
    u64 value;
    std::string string;
  };

  struct GroupId
  {
    void Set(u16 value);
    const char* c_str() const { return string.c_str(); }
    u16 value;
    std::string string;
  };

  std::string m_device_name;

  // Context title. Set on IOCTL_INIT to the ES active title, but can be changed afterwards
  // by ioctl 0x18.
  // TODO(wfs): implement ioctl 0x18.
  TitleId m_title_id;
  GroupId m_group_id;

  // Current active title (according to ES). Set on IOCTL_INIT.
  TitleId m_es_title_id;
  GroupId m_es_group_id;

  mbedtls_aes_context m_aes_ctx;
  u8 m_aes_key[0x10] = {};
  u8 m_aes_iv[0x10] = {};

  // TMD for the title that is being imported.
  IOS::ES::TMDReader m_import_tmd;
  std::string m_base_extract_path;

  // Set on IMPORT_TITLE_INIT when the next profile application should not delete
  // temporary install files.
  bool m_continue_install = false;

  // Set on IMPORT_TITLE_INIT to indicate that the install is a patch and not a
  // standalone title.
  enum class ImportType : s32
  {
    Title = 0,
    Patch = 1,
    Patch2 = 2,
    Invalid = -1,
  };
  ImportType m_patch_type = ImportType::Title;

  ARCUnpacker m_arc_unpacker;

  enum
  {
    IOCTL_WFSI_IMPORT_TITLE_INIT = 0x02,

    IOCTL_WFSI_PREPARE_CONTENT = 0x03,
    IOCTL_WFSI_IMPORT_CONTENT = 0x04,
    IOCTL_WFSI_FINALIZE_CONTENT = 0x05,

    IOCTL_WFSI_FINALIZE_IMPORT = 0x06,

    IOCTL_WFSI_DELETE_TITLE = 0x17,
    IOCTL_WFSI_IMPORT_TITLE_CANCEL = 0x2f,

    IOCTL_WFSI_INIT = 0x81,
    IOCTL_WFSI_SET_DEVICE_NAME = 0x82,

    IOCTL_WFSI_PREPARE_PROFILE = 0x86,
    IOCTL_WFSI_IMPORT_PROFILE = 0x87,
    IOCTL_WFSI_FINALIZE_PROFILE = 0x88,

    IOCTL_WFSI_APPLY_TITLE_PROFILE = 0x89,

    IOCTL_WFSI_GET_TMD = 0x8a,
    IOCTL_WFSI_GET_TMD_ABSOLUTE = 0x8b,

    IOCTL_WFSI_SET_FST_BUFFER = 0x8e,

    IOCTL_WFSI_LOAD_DOL = 0x90,

    IOCTL_WFSI_CHECK_HAS_SPACE = 0x95,
  };
};
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
