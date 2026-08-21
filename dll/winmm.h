#pragma once

#include "types.h"

namespace winmm {

UINT WINAPI timeBeginPeriod(UINT uPeriod);
UINT WINAPI timeEndPeriod(UINT uPeriod);
DWORD WINAPI timeGetTime();

} // namespace winmm
