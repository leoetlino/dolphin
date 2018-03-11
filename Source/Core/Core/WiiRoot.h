// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

namespace Core
{
void InitializeWiiRoot(bool use_temporary);
void ShutdownWiiRoot();

// Initialise or clean up the filesystem contents.
void InitializeFS();
void ShutdownFS();
}
