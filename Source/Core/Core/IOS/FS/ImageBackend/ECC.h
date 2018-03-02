// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace IOS::HLE::FS
{
using EccData = std::array<u8, 16>;

/// Calculate ECC data for 2048 bytes of data.
EccData CalculateEcc(const u8* data);

}  // namespace IOS::HLE::FS
