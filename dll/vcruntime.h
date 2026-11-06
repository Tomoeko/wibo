#pragma once

#include "types.h"

namespace vcruntime {

PVOID CDECL memchr(LPCVOID buffer, int ch, SIZE_T count);
LPSTR CDECL strstr(LPCSTR text, LPCSTR needle);
LPSTR CDECL strrchr(LPCSTR text, int ch);
PVOID CDECL memcpy(PVOID dest, LPCVOID src, SIZE_T count);
PVOID CDECL memset(PVOID dest, int ch, SIZE_T count);
int CDECL memcmp(LPCVOID buf1, LPCVOID buf2, SIZE_T count);
PVOID CDECL memmove(PVOID dest, LPCVOID src, SIZE_T count);

} // namespace vcruntime
