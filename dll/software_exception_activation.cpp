#include "software_exception_activation.h"

#ifdef WIBO_GUEST_64
#include "common.h"
#include "function_table.h"
#include "kernel32/vectored_exception.h"
#include "setup.h"

#include <cstring>
#include <type_traits>

namespace {
thread_local SoftwareExceptionActivation64 *g_currentActivation;
constexpr unsigned kActivationLimit = 1024;

bool validActivation(const SoftwareExceptionActivation64 *activation, ULONGLONG stackLimit, ULONGLONG stackBase) {
	const auto address = reinterpret_cast<ULONGLONG>(activation);
	return address >= stackLimit && address < stackBase && sizeof(*activation) <= stackBase - address &&
		   activation->stackLow >= stackLimit && activation->stackLow < activation->stackHigh &&
		   activation->stackHigh <= stackBase && address >= activation->stackLow && address < activation->stackHigh &&
		   sizeof(*activation) <= activation->stackHigh - address && activation->walkStart &&
		   reinterpret_cast<ULONGLONG>(activation->walkStart) >= activation->stackLow &&
		   reinterpret_cast<ULONGLONG>(activation->walkStart) < activation->stackHigh &&
		   sizeof(CONTEXT64) <= activation->stackHigh - reinterpret_cast<ULONGLONG>(activation->walkStart) &&
		   activation->reserved == 0 &&
		   ((!activation->bridgeRip && !activation->bridgeRsp &&
			 activation->phase == SoftwareExceptionActivationPhase64::TargetUnwind) ||
			((activation->bridgeRip == reinterpret_cast<ULONGLONG>(wiboFrameHandlerContinuation64) ||
			  (activation->phase == SoftwareExceptionActivationPhase64::Consolidation &&
			   activation->bridgeRip == reinterpret_cast<ULONGLONG>(wiboConsolidationContinuation64))) &&
			 activation->bridgeRsp == activation->stackLow && !(activation->bridgeRsp & 15))) &&
		   (activation->phase == SoftwareExceptionActivationPhase64::FrameSearch ||
			activation->phase == SoftwareExceptionActivationPhase64::TargetUnwind ||
			activation->phase == SoftwareExceptionActivationPhase64::Consolidation);
}
} // namespace

static_assert(std::is_trivially_copyable_v<SoftwareExceptionActivation64>);
static_assert(std::is_trivially_destructible_v<SoftwareExceptionActivation64>);

namespace wibo {
bool linkSoftwareExceptionActivation(SoftwareExceptionActivation64 *activation, const CONTEXT64 *walkStart,
									 ULONGLONG stackLow, ULONGLONG stackHigh,
									 SoftwareExceptionActivationPhase64 phase) {
	if (!activation || !walkStart || stackLow >= stackHigh ||
		(g_currentActivation && stackHigh > g_currentActivation->stackLow))
		return false;
	*activation = {g_currentActivation, walkStart, stackLow, stackHigh, 0, 0, phase, 0};
	g_currentActivation = activation;
	return true;
}

bool unlinkSoftwareExceptionActivation(SoftwareExceptionActivation64 *activation) {
	if (g_currentActivation != activation)
		return false;
	g_currentActivation = activation->previous;
	activation->previous = nullptr;
	return true;
}

SoftwareExceptionActivation64 *currentSoftwareExceptionActivation() { return g_currentActivation; }

SoftwareExceptionBridgeResult64 restartSoftwareExceptionAtBridge(CONTEXT64 &context, ULONGLONG targetFrame) {
	if (context.Rip != reinterpret_cast<ULONGLONG>(wiboFrameHandlerContinuation64) &&
		context.Rip != reinterpret_cast<ULONGLONG>(wiboConsolidationContinuation64))
		return SoftwareExceptionBridgeResult64::NoMatch;
#if defined(__APPLE__)
	const TEB *teb = currentTebForGuestTransition();
#else
	const TEB *teb = currentThreadTeb;
#endif
	if (!teb)
		return SoftwareExceptionBridgeResult64::Unsupported;
	unsigned count = 0;
	const SoftwareExceptionActivation64 *previous = nullptr;
	const SoftwareExceptionActivation64 *match = nullptr;
	for (auto *activation = g_currentActivation; activation; activation = activation->previous) {
		if (++count > kActivationLimit || !validActivation(activation, teb->Tib.StackLimit, teb->Tib.StackBase) ||
			(previous && previous->stackHigh > activation->stackLow))
			return SoftwareExceptionBridgeResult64::Unsupported;
		if (activation->bridgeRip == context.Rip && activation->bridgeRsp == context.Rsp)
			match = activation;
		previous = activation;
	}
	if (!match ||
		(match->phase != SoftwareExceptionActivationPhase64::FrameSearch &&
		 match->phase != SoftwareExceptionActivationPhase64::Consolidation) ||
		match->walkStart->Rsp <= context.Rsp || targetFrame < match->walkStart->Rsp)
		return SoftwareExceptionBridgeResult64::Unsupported;
	std::memcpy(&context, match->walkStart, sizeof(context));
	return SoftwareExceptionBridgeResult64::Restarted;
}
} // namespace wibo

namespace {
bool contextRestoreTransfer(ULONGLONG targetRsp, bool commit) {
	if (wibo::hasActiveVectoredExceptionTraversal() || wibo::hasActiveFunctionTableCallback())
		return false;
#if defined(__APPLE__)
	const TEB *teb = currentTebForGuestTransition();
#else
	const TEB *teb = currentThreadTeb;
#endif
	if (!teb || (targetRsp & 7) || targetRsp <= teb->Tib.StackLimit || targetRsp > teb->Tib.StackBase)
		return false;
	unsigned count = 0;
	const SoftwareExceptionActivation64 *previous = nullptr;
	for (auto *activation = g_currentActivation; activation; activation = activation->previous) {
		if (++count > kActivationLimit || !validActivation(activation, teb->Tib.StackLimit, teb->Tib.StackBase) ||
			(previous && previous->stackHigh > activation->stackLow) ||
			(targetRsp >= activation->stackLow && targetRsp < activation->stackHigh))
			return false;
		previous = activation;
	}
	if (!commit)
		return true;
	// Every owned scope was checked before mutating the chain. Only plain
	// assembly activations are crossed; native registry owners were rejected.
	while (g_currentActivation && targetRsp >= g_currentActivation->stackHigh) {
		auto *activation = g_currentActivation;
		g_currentActivation = activation->previous;
		activation->previous = nullptr;
	}
	return true;
}
} // namespace

bool wiboValidateContextRestoreTransfer64(ULONGLONG targetRsp) { return contextRestoreTransfer(targetRsp, false); }
bool wiboPrepareContextRestoreTransfer64(ULONGLONG targetRsp) { return contextRestoreTransfer(targetRsp, true); }
#endif
