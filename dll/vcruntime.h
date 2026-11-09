#pragma once

#include "types.h"

namespace vcruntime {

struct ExceptionData {
	GUEST_PTR what;
	bool doFree;
};

static_assert(offsetof(ExceptionData, doFree) == sizeof(GUEST_PTR));
static_assert(sizeof(ExceptionData) == sizeof(GUEST_PTR) * 2);

GUEST_PTR CDECL _set_se_translator(GUEST_PTR translator);
void CDECL __std_exception_copy(const ExceptionData *source, ExceptionData *destination);
void CDECL __std_exception_destroy(ExceptionData *data);

PVOID CDECL memchr(LPCVOID buffer, int ch, SIZE_T count);
LPSTR CDECL strstr(LPCSTR text, LPCSTR needle);
LPSTR CDECL strrchr(LPCSTR text, int ch);
LPSTR CDECL strchr(LPCSTR text, int ch);
PVOID CDECL memcpy(PVOID dest, LPCVOID src, SIZE_T count);
PVOID CDECL memset(PVOID dest, int ch, SIZE_T count);
int CDECL memcmp(LPCVOID buf1, LPCVOID buf2, SIZE_T count);
PVOID CDECL memmove(PVOID dest, LPCVOID src, SIZE_T count);

} // namespace vcruntime
