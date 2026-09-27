#pragma once

#include "context_x64.h"
#include "software_exception_activation_offsets.h"

#ifdef WIBO_GUEST_64
enum class SoftwareExceptionActivationPhase64 : DWORD {
	FrameSearch = 1,
	TargetUnwind,
};

struct SoftwareExceptionActivation64 {
	SoftwareExceptionActivation64 *previous;
	const CONTEXT64 *walkStart;
	ULONGLONG stackLow;
	ULONGLONG stackHigh;
	ULONGLONG bridgeRip;
	ULONGLONG bridgeRsp;
	SoftwareExceptionActivationPhase64 phase;
	DWORD reserved;
};

static_assert(sizeof(SoftwareExceptionActivation64) == 56);
static_assert(offsetof(SoftwareExceptionActivation64, stackLow) == WIBO_SOFTWARE_ACTIVATION_STACK_LOW);
static_assert(offsetof(SoftwareExceptionActivation64, bridgeRip) == WIBO_SOFTWARE_ACTIVATION_BRIDGE_RIP);
static_assert(offsetof(SoftwareExceptionActivation64, bridgeRsp) == WIBO_SOFTWARE_ACTIVATION_BRIDGE_RSP);

namespace wibo {
enum class SoftwareExceptionBridgeResult64 {
	NoMatch,
	Restarted,
	Unsupported,
};
bool linkSoftwareExceptionActivation(SoftwareExceptionActivation64 *activation, const CONTEXT64 *walkStart,
									 ULONGLONG stackLow, ULONGLONG stackHigh, SoftwareExceptionActivationPhase64 phase);
bool unlinkSoftwareExceptionActivation(SoftwareExceptionActivation64 *activation);
SoftwareExceptionActivation64 *currentSoftwareExceptionActivation();
SoftwareExceptionBridgeResult64 restartSoftwareExceptionAtBridge(CONTEXT64 &context, ULONGLONG targetFrame);
} // namespace wibo

// Native entry, called only after the restore consumer has completed every
// validation and snapshot. No activation is removed when validation fails.
extern "C" bool wiboPrepareContextRestoreTransfer64(ULONGLONG targetRsp);
extern "C" const BYTE wiboFrameHandlerContinuation64[];
#endif
