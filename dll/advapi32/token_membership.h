#pragma once

#include "types.h"

namespace advapi32 {

BOOL WINAPI CheckTokenMembership(HANDLE token, PSID sid, LPBOOL member);

} // namespace advapi32
