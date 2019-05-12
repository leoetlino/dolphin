// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

struct libusb_config_descriptor;
struct libusb_context;
struct libusb_device;

namespace LibusbUtils
{
template <typename T>
using UniquePtr = std::unique_ptr<T, void (*)(T*)>;

using Context = UniquePtr<libusb_context>;
Context MakeContext();

using ConfigDescriptor = UniquePtr<libusb_config_descriptor>;
ConfigDescriptor MakeConfigDescriptor(libusb_device* device, u8 config_num = 0);
}  // namespace LibusbUtils
