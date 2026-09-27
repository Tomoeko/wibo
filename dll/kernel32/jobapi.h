#pragma once

#include "types.h"

namespace kernel32 {
BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE job, BOOL *result);
}
