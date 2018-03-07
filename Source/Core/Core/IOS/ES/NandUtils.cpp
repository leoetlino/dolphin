// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"

namespace IOS
{
namespace HLE
{
namespace Device
{
template <typename BlobReader>
static BlobReader ReadFile(FS::FileSystem* fs, const std::string& path)
{
  const auto fd = fs->OpenFile(0, 0, path, FS::Mode::Read);
  if (!fd)
    return {};

  std::vector<u8> bytes(fs->GetFileStatus(*fd)->size);
  if (!fs->ReadFile(*fd, bytes.data(), bytes.size()))
    return {};

  return BlobReader{std::move(bytes)};
}

IOS::ES::TMDReader ES::FindImportTMD(u64 title_id) const
{
  return ReadFile<IOS::ES::TMDReader>(m_ios.GetFS().get(),
                                      Common::GetImportTitlePath(title_id) + "/content/title.tmd");
}

IOS::ES::TMDReader ES::FindInstalledTMD(u64 title_id) const
{
  return ReadFile<IOS::ES::TMDReader>(m_ios.GetFS().get(), Common::GetTMDFileName(title_id));
}

IOS::ES::TicketReader ES::FindSignedTicket(u64 title_id) const
{
  return ReadFile<IOS::ES::TicketReader>(m_ios.GetFS().get(), Common::GetTicketFileName(title_id));
}

static bool IsValidPartOfTitleID(const std::string& string)
{
  if (string.length() != 8)
    return false;
  return std::all_of(string.begin(), string.end(),
                     [](const auto character) { return std::isxdigit(character) != 0; });
}

static std::vector<u64> GetTitlesInTitleOrImport(FS::FileSystem* fs, const std::string& titles_dir)
{
  const auto entries = fs->ReadDirectory(0, 0, titles_dir);
  if (!entries)
  {
    ERROR_LOG(IOS_ES, "%s is not a directory", titles_dir.c_str());
    return {};
  }

  std::vector<u64> title_ids;

  // The /title and /import directories contain one directory per title type, and each of them has
  // a directory per title (where the name is the low 32 bits of the title ID in %08x format).
  for (const std::string& title_type : *entries)
  {
    const auto type_entries = fs->ReadDirectory(0, 0, titles_dir + "/" + title_type);
    if (!type_entries || !IsValidPartOfTitleID(title_type))
      continue;

    for (const std::string& title_identifier : *type_entries)
    {
      if (!fs->ReadDirectory(0, 0, titles_dir + "/" + title_type + "/" + title_identifier) ||
          !IsValidPartOfTitleID(title_identifier))
      {
        continue;
      }

      const u32 type = std::stoul(title_type, nullptr, 16);
      const u32 identifier = std::stoul(title_identifier, nullptr, 16);
      title_ids.push_back(static_cast<u64>(type) << 32 | identifier);
    }
  }

  return title_ids;
}

std::vector<u64> ES::GetInstalledTitles() const
{
  return GetTitlesInTitleOrImport(m_ios.GetFS().get(), "/title");
}

std::vector<u64> ES::GetTitleImports() const
{
  return GetTitlesInTitleOrImport(m_ios.GetFS().get(), "/import");
}

std::vector<u64> ES::GetTitlesWithTickets() const
{
  const auto entries = m_ios.GetFS()->ReadDirectory(0, 0, "/ticket");
  if (!entries)
  {
    ERROR_LOG(IOS_ES, "FS_ReadDir(/ticket) failed");
    return {};
  }

  std::vector<u64> title_ids;

  // The /ticket directory contains one directory per title type, and each of them contains
  // one ticket per title (where the name is the low 32 bits of the title ID in %08x format).
  for (const std::string& title_type : *entries)
  {
    const auto type_entries = m_ios.GetFS()->ReadDirectory(0, 0, "/ticket/" + title_type);
    if (!type_entries || !IsValidPartOfTitleID(title_type))
      continue;

    for (const std::string& file_name : *type_entries)
    {
      const std::string name_without_ext = file_name.substr(0, 8);
      if (m_ios.GetFS()->ReadDirectory(0, 0, "/ticket/" + title_type + "/" + file_name) ||
          !IsValidPartOfTitleID(name_without_ext) || name_without_ext + ".tik" != file_name)
      {
        continue;
      }

      const u32 type = std::stoul(title_type, nullptr, 16);
      const u32 identifier = std::stoul(name_without_ext, nullptr, 16);
      title_ids.push_back(static_cast<u64>(type) << 32 | identifier);
    }
  }

  return title_ids;
}

std::vector<IOS::ES::Content> ES::GetStoredContentsFromTMD(const IOS::ES::TMDReader& tmd) const
{
  if (!tmd.IsValid())
    return {};

  const IOS::ES::SharedContentMap map{m_ios.GetFS().get()};
  const std::vector<IOS::ES::Content> contents = tmd.GetContents();

  std::vector<IOS::ES::Content> stored_contents;

  std::copy_if(contents.begin(), contents.end(), std::back_inserter(stored_contents),
               [this, &tmd, &map](const IOS::ES::Content& content) {
                 const std::string path = GetContentPath(tmd.GetTitleId(), content, map);
                 return !path.empty() && m_ios.GetFS()->GetMetadata(0, 0, path).Succeeded();
               });

  return stored_contents;
}

u32 ES::GetSharedContentsCount() const
{
  const auto entries = m_ios.GetFS()->ReadDirectory(0, 0, "/shared1");
  if (!entries)
    return 0;
  return static_cast<u32>(
      std::count_if(entries->begin(), entries->end(), [this](const std::string& name) {
        const bool is_dir = m_ios.GetFS()->ReadDirectory(0, 0, "/shared1/" + name).Succeeded();
        return !is_dir && name.size() == 12 && name.compare(8, 4, ".app") == 0;
      }));
}

std::vector<std::array<u8, 20>> ES::GetSharedContents() const
{
  const IOS::ES::SharedContentMap map{m_ios.GetFS().get()};
  return map.GetHashes();
}

static bool DeleteDirectoriesIfEmpty(FS::FileSystem* fs, const std::string& path)
{
  std::string::size_type position = std::string::npos;
  do
  {
    const auto directory = fs->ReadDirectory(0, 0, path.substr(0, position));
    if ((directory && directory->empty()) ||
        (!directory && directory.Error() != FS::ResultCode::NotFound))
    {
      if (fs->Delete(0, 0, path.substr(0, position)) != FS::ResultCode::Success)
        return false;
    }
    position = path.find_last_of('/', position - 1);
  } while (position != 0);
}

bool ES::InitImport(const IOS::ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const std::string content_dir = Common::GetTitleContentPath(tmd.GetTitleId());
  const std::string import_content_dir = Common::GetImportTitlePath(tmd.GetTitleId()) + "/content";
  if (fs->CreateFullPath(0, 0, content_dir + "/", 0, FS::Mode::ReadWrite, FS::Mode::ReadWrite,
                         FS::Mode::Read) != FS::ResultCode::Success ||
      fs->SetMetadata(0, content_dir, 0, 0, 0, FS::Mode::ReadWrite, FS::Mode::ReadWrite,
                      FS::Mode::None) != FS::ResultCode::Success ||
      fs->CreateFullPath(0, 0, import_content_dir + "/", 0, FS::Mode::ReadWrite,
                         FS::Mode::ReadWrite, FS::Mode::None) != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "InitImport: Failed to create content dir for %016" PRIx64, tmd.GetTitleId());
    return false;
  }

  const std::string data_dir = Common::GetTitleDataPath(tmd.GetTitleId());
  const auto data_dir_contents = fs->ReadDirectory(0, 0, data_dir);
  if (!data_dir_contents &&
      (data_dir_contents.Error() != FS::ResultCode::NotFound ||
       fs->CreateDirectory(0, 0, data_dir, 0, FS::Mode::ReadWrite, FS::Mode::None,
                           FS::Mode::None) != FS::ResultCode::Success))
  {
    return false;
  }

  IOS::ES::UIDSys uid_sys{fs.get()};
  const u32 uid = uid_sys.GetOrInsertUIDForTitle(tmd.GetTitleId());
  if (fs->SetMetadata(0, data_dir, uid, tmd.GetGroupId(), 0, FS::Mode::ReadWrite, FS::Mode::None,
                      FS::Mode::None) != FS::ResultCode::Success)
  {
    return false;
  }

  // IOS moves the title content directory to /import if the TMD exists during an import.
  const auto file_info = fs->GetMetadata(0, 0, Common::GetTMDFileName(tmd.GetTitleId()));
  if (!file_info || !file_info->is_file)
    return true;

  const auto result = fs->Rename(0, 0, content_dir, import_content_dir);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "InitImport: Failed to move content dir for %016" PRIx64, tmd.GetTitleId());
    return false;
  }

  return true;
}

bool ES::FinishImport(const IOS::ES::TMDReader& tmd)
{
  const u64 title_id = tmd.GetTitleId();
  const std::string import_content_dir = Common::GetImportTitlePath(title_id) + "/content";

  // Remove everything not listed in the TMD.
  std::unordered_set<std::string> expected_entries = {"title.tmd"};
  for (const auto& content_info : tmd.GetContents())
    expected_entries.insert(StringFromFormat("%08x.app", content_info.id));
  const auto entries = m_ios.GetFS()->ReadDirectory(0, 0, import_content_dir);
  if (!entries)
    return false;
  for (const std::string& name : *entries)
  {
    const std::string absolute_path = import_content_dir + "/" + name;
    // There should not be any directory in there. Remove it.
    if (m_ios.GetFS()->ReadDirectory(0, 0, absolute_path))
      m_ios.GetFS()->Delete(0, 0, absolute_path);
    else if (expected_entries.find(name) == expected_entries.end())
      m_ios.GetFS()->Delete(0, 0, absolute_path);
  }

  const std::string content_dir = Common::GetTitleContentPath(title_id);
  if (m_ios.GetFS()->Rename(0, 0, import_content_dir, content_dir) != FS::ResultCode::Success)
  {
    ERROR_LOG(IOS_ES, "FinishImport: Failed to rename import directory to %s", content_dir.c_str());
    return false;
  }
  DeleteDirectoriesIfEmpty(m_ios.GetFS().get(), import_content_dir);
  return true;
}

bool ES::WriteImportTMD(const IOS::ES::TMDReader& tmd)
{
  const std::string tmd_path = "/tmp/title.tmd";
  m_ios.GetFS()->CreateFile(0, 0, tmd_path, 0, FS::Mode::ReadWrite, FS::Mode::ReadWrite,
                            FS::Mode::None);
  {
    const auto fd = m_ios.GetFS()->OpenFile(0, 0, tmd_path, FS::Mode::Write);
    if (!fd || !m_ios.GetFS()->WriteFile(*fd, tmd.GetBytes().data(), tmd.GetBytes().size()))
      return false;
  }

  const std::string dest = Common::GetImportTitlePath(tmd.GetTitleId()) + "/content/title.tmd";
  return m_ios.GetFS()->Rename(0, 0, tmd_path, dest) == FS::ResultCode::Success;
}

void ES::FinishStaleImport(u64 title_id)
{
  const auto import_tmd = FindImportTMD(title_id);
  if (!import_tmd.IsValid())
  {
    m_ios.GetFS()->Delete(0, 0, Common::GetImportTitlePath(title_id) + "/content");
    DeleteDirectoriesIfEmpty(m_ios.GetFS().get(), Common::GetImportTitlePath(title_id));
    DeleteDirectoriesIfEmpty(m_ios.GetFS().get(), Common::GetTitlePath(title_id));
  }
  else
  {
    FinishImport(import_tmd);
  }
}

void ES::FinishAllStaleImports()
{
  const std::vector<u64> titles = GetTitleImports();
  for (const u64& title_id : titles)
    FinishStaleImport(title_id);
}

std::string ES::GetContentPath(const u64 title_id, const IOS::ES::Content& content) const
{
  return GetContentPath(title_id, content, IOS::ES::SharedContentMap{m_ios.GetFS().get()});
}

std::string ES::GetContentPath(const u64 title_id, const IOS::ES::Content& content,
                               const IOS::ES::SharedContentMap& content_map) const
{
  if (content.IsShared())
    return content_map.GetFilenameFromSHA1(content.sha1).value_or("");

  return Common::GetTitleContentPath(title_id) + StringFromFormat("/%08x.app", content.id);
}
}  // namespace Device
}  // namespace HLE
}  // namespace IOS
