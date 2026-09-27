#include "software_exception_decision.h"

#include "kernel32/vectored_exception.h"

#include <cstring>

#ifdef WIBO_GUEST_64
namespace {
constexpr DWORD kNoncontinuable = 1;
constexpr DWORD kNoncontinuableException = 0xc0000025;
constexpr DWORD kInvalidParameter = 0xc000000d;
constexpr DWORD kLegacyThreadNameException = 0x406d1388;
constexpr DWORD kMutableArithmeticFlags = 0x8d5;
constexpr WORD kX87ExceptionMasks = 0x3f;
constexpr DWORD kSseExceptionMasks = 0x1f80;

bool supportedContext(const CONTEXT64 &selected, const CONTEXT64 &original) {
	const bool segmentsChanged = selected.SegCs != original.SegCs || selected.SegSs != original.SegSs ||
								 selected.SegDs != original.SegDs || selected.SegEs != original.SegEs ||
								 selected.SegFs != original.SegFs || selected.SegGs != original.SegGs;
	const DWORD mxcsrMask = original.FltSave.MxCsrMask;
	const bool mxcsrUnsupported =
		mxcsrMask ? (selected.FltSave.MxCsr & ~mxcsrMask) != 0 : selected.FltSave.MxCsr != original.FltSave.MxCsr;
	// The verified subset requires masked legacy floating-point exceptions.
	// Unsupported control values are rejected rather than silently normalized.
	const bool exceptionsUnmasked = (selected.FltSave.ControlWord & kX87ExceptionMasks) != kX87ExceptionMasks ||
									(selected.FltSave.MxCsr & kSseExceptionMasks) != kSseExceptionMasks;
	return selected.ContextFlags == WIBO_CTX64_CAPTURE_FLAGS && !segmentsChanged && !mxcsrUnsupported &&
		   !exceptionsUnmasked && ((selected.EFlags ^ original.EFlags) & ~kMutableArithmeticFlags) == 0;
}
} // namespace

DWORD wiboPrepareSoftwareExceptionDecision64(const SoftwareExceptionCapture64 *capture,
											 SoftwareExceptionDecision64 *output) {
	*output = {};
	output->kind = SoftwareExceptionDecisionKind64::InvalidCapture;
	output->failureCode = kInvalidParameter;
	if (!capture || !capture->record) {
		return static_cast<DWORD>(output->kind);
	}
	output->originalCode = capture->record->ExceptionCode;
	output->originalFlags = capture->record->ExceptionFlags;
	output->failureCode = output->originalCode;
	std::memcpy(&output->resumeContext, &capture->context, sizeof(CONTEXT64));
	EXCEPTION_POINTERS info{toGuestPtr(capture->record), toGuestPtr(&output->resumeContext)};

	// This traversal returns with its entry references and lock released. The
	// mutable context is separate from all immutable ancestry snapshots.
	const LONG result = wibo::invokeVectoredExceptionHandlers(&info, wibo::invokeVectoredGuestHandler64);
	if (result != EXCEPTION_CONTINUE_EXECUTION) {
		output->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
		if (result == EXCEPTION_CONTINUE_SEARCH && output->originalCode == kLegacyThreadNameException &&
			((output->originalFlags | capture->record->ExceptionFlags) & kNoncontinuable) == 0 &&
			capture->callerCapture && capture->record == &capture->callerCapture->localRecord) {
			// Preserve the notification return policy using the immutable capture.
			// Context mutations from unsuccessful handlers do not select a resume.
			std::memcpy(&output->resumeContext, &capture->context, sizeof(CONTEXT64));
			output->kind = SoftwareExceptionDecisionKind64::LegacyNotificationReturn;
			output->failureCode = 0;
		}
		return static_cast<DWORD>(output->kind);
	}
	// Neither clearing an original noncontinuable flag nor setting it while
	// handling permits continuation in this initial decision subset.
	if ((output->originalFlags | capture->record->ExceptionFlags) & kNoncontinuable) {
		output->kind = SoftwareExceptionDecisionKind64::NoncontinuableContinuationRequiresDispatch;
		output->failureCode = kNoncontinuableException;
		return static_cast<DWORD>(output->kind);
	}
	auto &selected = output->resumeContext;
	const auto &original = capture->context;
	// Preserve the captured continuation geometry. An arbitrary target could
	// abandon an outer callback's still-live host traversal activation.
	if (selected.Rip != original.Rip || selected.Rsp != original.Rsp) {
		output->kind = SoftwareExceptionDecisionKind64::UnsupportedControlTransfer;
		return static_cast<DWORD>(output->kind);
	}
	if (!supportedContext(selected, original) || info.ExceptionRecord != toGuestPtr(capture->record) ||
		info.ContextRecord != toGuestPtr(&selected)) {
		output->kind = SoftwareExceptionDecisionKind64::UnsupportedContextState;
		return static_cast<DWORD>(output->kind);
	}
	// The raw legacy restore uses FltSave.MxCsr, matching the installed oracle.
	// Divergent MXCSR field authority on other implementations is unverified.
	output->kind = SoftwareExceptionDecisionKind64::Resume;
	output->failureCode = 0;
	return static_cast<DWORD>(output->kind);
}
#endif
