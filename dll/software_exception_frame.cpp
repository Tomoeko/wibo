#include "software_exception_frame.h"

#ifdef WIBO_GUEST_64
#include "common.h"
#include "heap.h"
#include "setup.h"
#include "software_exception_dispatch_offsets.h"
#include "virtual_unwind.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
constexpr DWORD kExceptionHandler = 1;
constexpr DWORD kFrameLimit = 1024;
constexpr LONG kContinueExecution = 0, kContinueSearch = 1;

bool readableStack(ULONGLONG address, size_t size, const SoftwareExceptionFrameActivation64 &activation) {
	if (address < activation.stackLimit || address >= activation.stackBase || size > activation.stackBase - address)
		return false;
	while (size) {
		MEMORY_BASIC_INFORMATION region{};
		if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) !=
				wibo::heap::VmStatus::Success ||
			region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			(region.Protect & 0xff) == PAGE_EXECUTE || address < region.BaseAddress ||
			address - region.BaseAddress >= region.RegionSize)
			return false;
		const auto available = region.RegionSize - (address - region.BaseAddress);
		const auto amount = std::min<size_t>(size, available);
		size -= amount;
		address += amount;
	}
	return true;
}

bool guestExecutableAddress(ULONGLONG address) {
	MEMORY_BASIC_INFORMATION region{};
	if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) != wibo::heap::VmStatus::Success ||
		region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return false;
	const DWORD protection = region.Protect & 0xff;
	return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
		   protection == PAGE_EXECUTE_WRITECOPY;
}

// Lookup callbacks and the unwinder finish their native ownership scopes
// before this step returns. No registry lock survives a personality call.
bool prepareFrame(SoftwareExceptionFrameActivation64 &activation, DWORD handlerType, PVOID historyTable = nullptr) {
	auto &context = activation.walkingContext;
	const ULONGLONG previousRsp = context.Rsp;
	if ((previousRsp & 7) || !readableStack(previousRsp, sizeof(ULONGLONG), activation)) {
		DEBUG_LOG("software frame search: invalid stack rip=%llx rsp=%llx\n", context.Rip, previousRsp);
		return false;
	}
	if (!guestExecutableAddress(context.Rip)) {
		DEBUG_LOG("software frame search: unknown guest code rip=%llx rsp=%llx\n", context.Rip, previousRsp);
		return false;
	}
	activation.frameContext = context;
	auto &dispatcher = activation.dispatcher;
	dispatcher = {};
	dispatcher.ControlPc = context.Rip;
	dispatcher.ContextRecord = &context;
	dispatcher.HistoryTable = historyTable;
	dispatcher.FunctionEntry = ntdll::RtlLookupFunctionEntry(context.Rip, &dispatcher.ImageBase, historyTable);
	if (dispatcher.FunctionEntry) {
		activation.function = *dispatcher.FunctionEntry;
		if (activation.function.BeginAddress >= activation.function.EndAddress || context.Rip < dispatcher.ImageBase ||
			context.Rip - dispatcher.ImageBase < activation.function.BeginAddress ||
			context.Rip - dispatcher.ImageBase >= activation.function.EndAddress) {
			DEBUG_LOG("software frame search: invalid function entry pc=%llx base=%llx\n", context.Rip,
					  dispatcher.ImageBase);
			return false;
		}
		PVOID handler = nullptr;
		if (!wibo::virtualUnwindWithStackBounds(handlerType, dispatcher.ImageBase, dispatcher.ControlPc,
												&activation.function, &context, &dispatcher.HandlerData,
												&dispatcher.EstablisherFrame, activation.stackLimit,
												activation.stackBase, &handler)) {
			DEBUG_LOG("software frame search: bounded unwind rejected pc=%llx base=%llx\n", dispatcher.ControlPc,
					  dispatcher.ImageBase);
			return false;
		}
		dispatcher.LanguageHandler = reinterpret_cast<SoftwareFrameHandler64>(handler);
	} else {
		// Only a known guest executable mapping may use the leaf-function rule.
		// Unregistered native host frames are not Windows leaf frames.
		dispatcher.EstablisherFrame = previousRsp;
		std::memcpy(&context.Rip, reinterpret_cast<const void *>(previousRsp), sizeof(context.Rip));
		context.Rsp += sizeof(ULONGLONG);
	}
	const bool knownHandler =
		!dispatcher.LanguageHandler || guestExecutableAddress(reinterpret_cast<ULONGLONG>(dispatcher.LanguageHandler));
	const bool valid = knownHandler && dispatcher.EstablisherFrame >= activation.stackLimit &&
					   dispatcher.EstablisherFrame < activation.stackBase && !(dispatcher.EstablisherFrame & 7) &&
					   context.Rsp > previousRsp && context.Rsp <= activation.stackBase && !(context.Rsp & 7);
	DEBUG_LOG("software frame search: pc=%llx base=%llx frame=%llx handler=%p next-rip=%llx next-rsp=%llx valid=%u\n",
			  dispatcher.ControlPc, dispatcher.ImageBase, dispatcher.EstablisherFrame,
			  reinterpret_cast<void *>(dispatcher.LanguageHandler), context.Rip, context.Rsp, valid);
	return valid;
}

LONG invokeFrameHandler(SoftwareFrameHandler64 handler, EXCEPTION_RECORD *record, ULONGLONG frame,
						CONTEXT64 *originalContext, SoftwareDispatcherContext64 *dispatcher,
						SoftwareExceptionActivation64 *activation) {
	const auto stackLow = activation->stackLow;
	const auto bridgeRip = activation->bridgeRip;
	const auto bridgeRsp = activation->bridgeRsp;
#if defined(__APPLE__)
	TEB *teb = currentTebForGuestTransition();
	enterGuestContext(teb);
#endif
	const LONG result = wiboCallFrameHandler64(handler, record, frame, originalContext, dispatcher, activation);
#if defined(__APPLE__)
	enterHostContext();
#endif
	// A returning callback no longer owns its bridge stack. Nonlocal transfers
	// leave this plain scope through the assembly activation cleanup gate.
	activation->stackLow = stackLow;
	activation->bridgeRip = bridgeRip;
	activation->bridgeRsp = bridgeRsp;
	return result;
}
} // namespace

static_assert(std::is_trivially_copyable_v<SoftwareExceptionFrameActivation64>);
static_assert(std::is_trivially_destructible_v<SoftwareExceptionFrameActivation64>);

namespace wibo {
bool prepareSoftwareExceptionFrame64(SoftwareExceptionFrameActivation64 &activation, DWORD handlerType,
									 PVOID historyTable) {
	return prepareFrame(activation, handlerType, historyTable);
}

LONG invokeSoftwareExceptionFrameHandler64(SoftwareFrameHandler64 handler, EXCEPTION_RECORD *record, ULONGLONG frame,
										   CONTEXT64 *context, SoftwareDispatcherContext64 *dispatcher,
										   SoftwareExceptionActivation64 *activation) {
	return invokeFrameHandler(handler, record, frame, context, dispatcher, activation);
}
} // namespace wibo

void wiboSearchSoftwareExceptionFrames64(const SoftwareExceptionCapture64 *capture,
										 SoftwareExceptionDecision64 *decision,
										 SoftwareExceptionFrameActivation64 *activation) {
	*activation = {};
#if defined(__APPLE__)
	TEB *teb = currentTebForGuestTransition();
#else
	TEB *teb = currentThreadTeb;
#endif
	if (!teb)
		return;
	activation->stackLimit = teb->Tib.StackLimit;
	activation->stackBase = teb->Tib.StackBase;
	if (activation->stackLimit >= activation->stackBase)
		return;
	// Exact capture ancestry skips the native Raise wrapper without inventing
	// unwind metadata for it. Handler argument 3 remains the original context.
	std::memcpy(&activation->walkingContext,
				capture->callerCapture ? &capture->callerCapture->context : &capture->context, sizeof(CONTEXT64));
	for (; activation->frameCount < kFrameLimit; ++activation->frameCount) {
		const auto bridge =
			wibo::restartSoftwareExceptionAtBridge(activation->walkingContext, std::numeric_limits<ULONGLONG>::max());
		if (bridge == wibo::SoftwareExceptionBridgeResult64::Unsupported) {
			decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
			return;
		}
		if (!prepareFrame(*activation, kExceptionHandler))
			return;
		auto &dispatcher = activation->dispatcher;
		if (dispatcher.LanguageHandler) {
			const auto expectedDispatcher = dispatcher;
			const ULONGLONG expectedRip = activation->walkingContext.Rip;
			const ULONGLONG expectedRsp = activation->walkingContext.Rsp;
			const auto stackLow = reinterpret_cast<ULONGLONG>(activation) - WIBO_SOFTWARE_DISPATCH_FRAME_ACTIVATION;
			const auto *walkStart = capture->callerCapture ? &capture->callerCapture->context : &capture->context;
			if (!wibo::linkSoftwareExceptionActivation(&activation->activation, walkStart, stackLow, walkStart->Rsp,
													   SoftwareExceptionActivationPhase64::FrameSearch)) {
				decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
				return;
			}
			const LONG result =
				invokeFrameHandler(dispatcher.LanguageHandler, capture->record, dispatcher.EstablisherFrame,
								   &decision->resumeContext, &dispatcher, &activation->activation);
			if (!wibo::unlinkSoftwareExceptionActivation(&activation->activation)) {
				decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
				return;
			}
			DEBUG_LOG("software frame handler: pc=%llx frame=%llx result=%d\n", expectedDispatcher.ControlPc,
					  expectedDispatcher.EstablisherFrame, result);
			if (!wibo::softwareDispatcherControlUnchanged64(dispatcher, expectedDispatcher) ||
				activation->walkingContext.Rip != expectedRip || activation->walkingContext.Rsp != expectedRsp) {
				DEBUG_LOG("software frame search: unsupported dispatcher or walking-context mutation mask=%x\n",
						  wibo::softwareDispatcherMutationMask64(dispatcher, expectedDispatcher));
				decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
				return;
			}
			if (result == kContinueExecution) {
				wiboSelectSoftwareExceptionContinuation64(capture, decision);
				return;
			}
			if (result != kContinueSearch) {
				DEBUG_LOG("software frame search: unsupported handler disposition=%d\n", result);
				decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
				return;
			}
		}
		if (activation->walkingContext.Rsp == activation->stackBase) {
			return;
		}
	}
	DEBUG_LOG("software frame search: frame limit reached\n");
}
#endif
