#pragma once

#include "types.h"

namespace kernel32 {

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);
BOOL WINAPI QueryThreadCycleTime(HANDLE thread, ULONGLONG *cycles);

} // namespace kernel32
