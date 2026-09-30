#pragma once

#ifndef WIBO_GUEST_64
#include "kernel32/errhandlingapi.h"
#include "kernel32/thread_context.h"
#include "software_exception_x86_offsets.h"

struct SoftwareExceptionGuard32 {
	GUEST_PTR next, handler, protectedFrame;
	DWORD phase;
};

// This object lives entirely on the guest stack. Native helpers complete before
// a personality is called, so a CRT handler may abandon the object safely.
struct alignas(16) SoftwareExceptionFrame32 {
	DWORD capturedMxcsrMask;
	CONTEXT32 context;
	EXCEPTION_RECORD record, nestedRecord;
	GUEST_PTR current, handler, dispatcher;
	DWORD visited, phase;
	GUEST_PTR target, targetIp;
	DWORD returnValue;
	GUEST_PTR recordPointer, nestedFloor;
	SoftwareExceptionGuard32 guard;
	GUEST_PTR savedStack;
	DWORD restarts;
	DWORD handlerKind;
	EXCEPTION_POINTERS pointers;
	DWORD segments[6];
};

#define WIBO_CHECK_SEH32(field, offset) static_assert(offsetof(SoftwareExceptionFrame32, field) == offset)
WIBO_CHECK_SEH32(context, WIBO_SEH32_CONTEXT);
WIBO_CHECK_SEH32(record, WIBO_SEH32_RECORD);
WIBO_CHECK_SEH32(nestedRecord, WIBO_SEH32_NESTED_RECORD);
WIBO_CHECK_SEH32(current, WIBO_SEH32_CURRENT);
WIBO_CHECK_SEH32(handler, WIBO_SEH32_HANDLER);
WIBO_CHECK_SEH32(dispatcher, WIBO_SEH32_DISPATCHER);
WIBO_CHECK_SEH32(visited, WIBO_SEH32_VISITED);
WIBO_CHECK_SEH32(phase, WIBO_SEH32_PHASE);
WIBO_CHECK_SEH32(target, WIBO_SEH32_TARGET);
WIBO_CHECK_SEH32(targetIp, WIBO_SEH32_TARGET_IP);
WIBO_CHECK_SEH32(returnValue, WIBO_SEH32_RETURN);
WIBO_CHECK_SEH32(recordPointer, WIBO_SEH32_RECORD_POINTER);
WIBO_CHECK_SEH32(nestedFloor, WIBO_SEH32_NESTED_FLOOR);
WIBO_CHECK_SEH32(guard, WIBO_SEH32_GUARD);
WIBO_CHECK_SEH32(savedStack, WIBO_SEH32_SAVED_STACK);
WIBO_CHECK_SEH32(restarts, WIBO_SEH32_RESTARTS);
WIBO_CHECK_SEH32(handlerKind, WIBO_SEH32_HANDLER_KIND);
WIBO_CHECK_SEH32(pointers, WIBO_SEH32_POINTERS);
WIBO_CHECK_SEH32(segments, WIBO_SEH32_SEGMENTS);
#undef WIBO_CHECK_SEH32
static_assert(sizeof(SoftwareExceptionFrame32) == WIBO_SEH32_SIZE);
static_assert(sizeof(CONTEXT32) == 716 && sizeof(EXCEPTION_RECORD) == 80);
static_assert(offsetof(CONTEXT32, Eip) == WIBO_CTX32_EIP);
static_assert(offsetof(CONTEXT32, Esp) == WIBO_CTX32_ESP);
static_assert(offsetof(CONTEXT32, ExtendedRegisters) == WIBO_CTX32_EXTENDED);
static_assert((WIBO_SEH32_CONTEXT + WIBO_CTX32_EXTENDED) % 16 == 0);

extern "C" {
void wiboRaiseException32();
void wiboRtlRaiseException32();
void wiboRtlUnwind32();
void wiboExceptionGuard32();
}
#endif
