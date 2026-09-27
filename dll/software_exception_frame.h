#pragma once

#include "ntdll.h"
#include "software_exception_activation.h"
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
	SoftwareExceptionActivation64 activation;
};

static_assert(sizeof(SoftwareExceptionFrameActivation64) == 2640);

namespace wibo {
bool prepareSoftwareExceptionFrame64(SoftwareExceptionFrameActivation64 &activation, DWORD handlerType);
LONG invokeSoftwareExceptionFrameHandler64(SoftwareFrameHandler64 handler, EXCEPTION_RECORD *record, ULONGLONG frame,
										   CONTEXT64 *context, SoftwareDispatcherContext64 *dispatcher,
										   SoftwareExceptionActivation64 *activation);
} // namespace wibo

extern "C" {
// Native SysV entry. Nonreturning target unwind transfers cross only validated
// assembly activations; other handler dispositions remain unsupported.
void wiboSearchSoftwareExceptionFrames64(const SoftwareExceptionCapture64 *capture,
										 SoftwareExceptionDecision64 *decision,
										 SoftwareExceptionFrameActivation64 *activation);
LONG wiboCallFrameHandler64(SoftwareFrameHandler64 handler, EXCEPTION_RECORD *record, ULONGLONG frame,
							CONTEXT64 *originalContext, SoftwareDispatcherContext64 *dispatcher,
							SoftwareExceptionActivation64 *activation);
extern const BYTE wiboFrameHandlerContinuation64[];
}
#endif
