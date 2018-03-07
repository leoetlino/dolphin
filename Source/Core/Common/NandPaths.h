// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
// Returns /import/%08x/%08x. Intended for use by ES.
std::string GetImportTitlePath(u64 title_id);

std::string GetTicketFileName(u64 title_id);
std::string GetTitlePath(u64 title_id);
std::string GetTitleDataPath(u64 title_id);
std::string GetTitleContentPath(u64 title_id);
std::string GetTMDFileName(u64 title_id);

// Returns whether a path is within an installed title's directory.
bool IsTitlePath(const std::string& path, u64* title_id = nullptr);

// Escapes characters that are invalid or have special meanings in the host file system
std::string EscapeFileName(const std::string& filename);
// Escapes characters that are invalid or have special meanings in the host file system
std::string EscapePath(const std::string& path);
// Reverses escaping done by EscapeFileName
std::string UnescapeFileName(const std::string& filename);
}
