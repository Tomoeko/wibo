#pragma once

#include "ntdll.h"
#include "software_exception_decision.h"

#ifdef WIBO_GUEST_64
struct SoftwareDispatcherContext64;
using SoftwareFrameHandler64 = LONG(GUEST_STDCALL *)(EXCEPTION_RECORD *, ULONGLONG, CONTEXT64 *,
													 SoftwareDispatcherContext64 *);

struct SoftwareDispatcherContext64 {
	ULONGLONG ControlPc;
	ULONGLONG ImageBase;
	RUNTIME_FUNCTION *FunctionEntry;
	ULONGLONG EstablisherFrame;
	ULONGLONG TargetIp;
	CONTEXT64 *ContextRecord;
	SoftwareFrameHandler64 LanguageHandler;
	PVOID HandlerData;
	PVOID HistoryTable;
	DWORD ScopeIndex;
	DWORD Fill0;
};

static_assert(sizeof(SoftwareDispatcherContext64) == 80);
static_assert(offsetof(SoftwareDispatcherContext64, ContextRecord) == 40);
static_assert(offsetof(SoftwareDispatcherContext64, HistoryTable) == 64);
static_assert(offsetof(SoftwareDispatcherContext64, ScopeIndex) == 72);

// Assembly owns this plain activation for the entire search. Registered
// function-table storage remains owned by the registering guest runtime.
struct alignas(16) SoftwareExceptionFrameActivation64 {
	CONTEXT64 walkingContext;
	CONTEXT64 frameContext;
	SoftwareDispatcherContext64 dispatcher;
	RUNTIME_FUNCTION function;
	DWORD frameCount;
	ULONGLONG stackLimit;
	ULONGLONG stackBase;
};

static_assert(sizeof(SoftwareExceptionFrameActivation64) == 2576);

extern "C" {
// Native SysV entry; all callbacks must return normally in this initial stage.
void wiboSearchSoftwareExceptionFrames64(const SoftwareExceptionCapture64 *capture,
										 SoftwareExceptionDecision64 *decision,
										 SoftwareExceptionFrameActivation64 *activation);
LONG wiboCallFrameHandler64(SoftwareFrameHandler64 handler, EXCEPTION_RECORD *record, ULONGLONG frame,
							CONTEXT64 *originalContext, SoftwareDispatcherContext64 *dispatcher);
}
#endif
