#pragma once

#include "types.h"

namespace advapi32 {

BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD desiredAccess, BOOL openAsSelf, PHANDLE token);

} // namespace advapi32
