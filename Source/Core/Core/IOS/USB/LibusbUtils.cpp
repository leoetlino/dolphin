// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#if defined(__LIBUSB__)
#include <libusb.h>
#endif

#include "Core/IOS/USB/LibusbUtils.h"

namespace LibusbUtils
{
Context MakeContext()
{
#if defined(__LIBUSB__)
  libusb_context* context = nullptr;
  const int ret = libusb_init(&context);
  if (ret == LIBUSB_SUCCESS)
  {
    return {context, libusb_exit};
  }
#endif
  return {nullptr, [](auto* ptr) {}};
}

ConfigDescriptor MakeConfigDescriptor(libusb_device* device, u8 config_num)
{
#if defined(__LIBUSB__)
  libusb_config_descriptor* descriptor = nullptr;
  if (libusb_get_config_descriptor(device, config_num, &descriptor) == LIBUSB_SUCCESS)
    return {descriptor, libusb_free_config_descriptor};
#endif
  return {nullptr, [](auto* ptr) {}};
}
}  // namespace LibusbUtils
