#pragma once

#include "types.h"

namespace kernel32 {

HRESULT WINAPI SetThreadDescription(HANDLE handle, LPCWSTR description);
HRESULT WINAPI GetThreadDescription(HANDLE handle, GUEST_PTR *description);

} // namespace kernel32
