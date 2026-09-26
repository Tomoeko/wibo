#pragma once
#include "types.h"

struct alignas(16) M128A {
	ULONGLONG Low;
	LONGLONG High;
};
struct alignas(16) XMM_SAVE_AREA32 {
	WORD ControlWord, StatusWord;
	BYTE TagWord, Reserved1;
	WORD ErrorOpcode;
	DWORD ErrorOffset;
	WORD ErrorSelector, Reserved2;
	DWORD DataOffset;
	WORD DataSelector, Reserved3;
	DWORD MxCsr, MxCsrMask;
	M128A FloatRegisters[8];
	M128A XmmRegisters[16];
	BYTE Reserved4[96];
};
struct alignas(16) CONTEXT64 {
	ULONGLONG Home[6];
	DWORD ContextFlags, MxCsr;
	WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
	DWORD EFlags;
	ULONGLONG Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
	ULONGLONG Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
	ULONGLONG R8, R9, R10, R11, R12, R13, R14, R15, Rip;
	XMM_SAVE_AREA32 FltSave;
	M128A VectorRegister[26];
	ULONGLONG VectorControl, DebugControl;
	ULONGLONG LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
};
struct FLOATING_SAVE_AREA32 {
	DWORD ControlWord, StatusWord, TagWord, ErrorOffset, ErrorSelector, DataOffset, DataSelector;
	BYTE RegisterArea[80];
	DWORD Cr0NpxState;
};
struct CONTEXT32 {
	DWORD ContextFlags;
	DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
	FLOATING_SAVE_AREA32 FloatSave;
	DWORD SegGs, SegFs, SegEs, SegDs;
	DWORD Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs;
	BYTE ExtendedRegisters[512];
};
#ifdef WIBO_GUEST_64
struct KNONVOLATILE_CONTEXT_POINTERS {
	GUEST_PTR FloatingContext[16];
	GUEST_PTR IntegerContext[16];
};
using CONTEXT = CONTEXT64;
constexpr DWORD CONTEXT_ARCH = 0x100000;
#else
using CONTEXT = CONTEXT32;
constexpr DWORD CONTEXT_ARCH = 0x10000;
#endif
using LPCONTEXT = CONTEXT *;
constexpr DWORD CONTEXT_CONTROL = CONTEXT_ARCH | 1;
constexpr DWORD CONTEXT_INTEGER = CONTEXT_ARCH | 2;
constexpr DWORD CONTEXT_SEGMENTS = CONTEXT_ARCH | 4;
constexpr DWORD CONTEXT_FLOATING_POINT = CONTEXT_ARCH | 8;
constexpr DWORD CONTEXT_DEBUG_REGISTERS = CONTEXT_ARCH | 0x10;
