#pragma once

#include "types.h"

namespace kernel32 {

VOID WINAPI DebugBreak();
BOOL WINAPI IsDebuggerPresent();
VOID WINAPI OutputDebugStringA(LPCSTR lpOutputString);
VOID WINAPI OutputDebugStringW(LPCWSTR lpOutputString);
WORD WINAPI RtlCaptureStackBackTrace(DWORD FramesToSkip, DWORD FramesToCapture, PVOID *BackTrace,
									 PDWORD BackTraceHash);

} // namespace kernel32
