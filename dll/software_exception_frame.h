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
constexpr DWORD kSoftwareDispatcherScopeIndexMutation64 = 1u << 9;

inline DWORD softwareDispatcherMutationMask64(const SoftwareDispatcherContext64 &actual,
											  const SoftwareDispatcherContext64 &expected) {
	return (actual.ControlPc != expected.ControlPc ? 1u << 0 : 0) |
		   (actual.ImageBase != expected.ImageBase ? 1u << 1 : 0) |
		   (actual.FunctionEntry != expected.FunctionEntry ? 1u << 2 : 0) |
		   (actual.EstablisherFrame != expected.EstablisherFrame ? 1u << 3 : 0) |
		   (actual.TargetIp != expected.TargetIp ? 1u << 4 : 0) |
		   (actual.ContextRecord != expected.ContextRecord ? 1u << 5 : 0) |
		   (actual.LanguageHandler != expected.LanguageHandler ? 1u << 6 : 0) |
		   (actual.HandlerData != expected.HandlerData ? 1u << 7 : 0) |
		   (actual.HistoryTable != expected.HistoryTable ? 1u << 8 : 0) |
		   (actual.ScopeIndex != expected.ScopeIndex ? kSoftwareDispatcherScopeIndexMutation64 : 0) |
		   (actual.Fill0 != expected.Fill0 ? 1u << 10 : 0);
}

inline bool softwareDispatcherControlUnchanged64(const SoftwareDispatcherContext64 &actual,
												 const SoftwareDispatcherContext64 &expected) {
	// Language handlers advance this scope cursor before invoking cleanup.
	// Every control address and ownership pointer remains part of the contract.
	return (softwareDispatcherMutationMask64(actual, expected) & ~kSoftwareDispatcherScopeIndexMutation64) == 0;
}

bool prepareSoftwareExceptionFrame64(SoftwareExceptionFrameActivation64 &activation, DWORD handlerType,
									 PVOID historyTable = nullptr);
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
