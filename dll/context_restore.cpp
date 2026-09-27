#include "context_restore.h"

#ifdef WIBO_GUEST_64
#include "heap.h"
#include "kernel32/internal.h"
#include "kernel32/vectored_exception.h"
#include "setup.h"
#include "software_exception_activation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
constexpr DWORD kInvalidParameter = 0xc000000d;
constexpr DWORD kNotSupported = 0xc00000bb;
constexpr DWORD kConsolidate = 0x80000029;
constexpr DWORD kCollidedUnwind = 0x40;
constexpr DWORD kMutableArithmeticFlags = 0x8d5;
constexpr WORD kX87ExceptionMasks = 0x3f;
constexpr DWORD kSseExceptionMasks = 0x1f80;

bool accessibleRange(ULONGLONG address, size_t length, bool writable) {
	if (!address || !length || length > std::numeric_limits<ULONGLONG>::max() - address)
		return false;
	while (length) {
		MEMORY_BASIC_INFORMATION region{};
		if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) !=
				wibo::heap::VmStatus::Success ||
			region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
			address < region.BaseAddress || address - region.BaseAddress >= region.RegionSize)
			return false;
		const DWORD protection = region.Protect & 0xff;
		const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
							  protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
		const bool canRead = canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
		if (writable ? !canWrite : !canRead)
			return false;
		const auto available = region.RegionSize - (address - region.BaseAddress);
		const auto amount = std::min<size_t>(length, available);
		length -= amount;
		address += amount;
	}
	return true;
}

bool executableTarget(ULONGLONG address) {
	MEMORY_BASIC_INFORMATION region{};
	if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) != wibo::heap::VmStatus::Success ||
		region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
		return false;
	const DWORD protection = region.Protect & 0xff;
	return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
		   protection == PAGE_EXECUTE_WRITECOPY;
}

bool supportedLegacyState(const CONTEXT64 &selected, const CONTEXT64 &current) {
	if (selected.ContextFlags != WIBO_CTX64_CAPTURE_FLAGS)
		return false;
	if (selected.SegCs != current.SegCs || selected.SegSs != current.SegSs || selected.SegDs != current.SegDs ||
		selected.SegEs != current.SegEs || selected.SegFs != current.SegFs || selected.SegGs != current.SegGs ||
		((selected.EFlags ^ current.EFlags) & ~kMutableArithmeticFlags))
		return false;
	const DWORD mxcsr = selected.FltSave.MxCsr;
	const DWORD mask = current.FltSave.MxCsrMask;
	// The tested subset has consistent MXCSR mirrors and masked FP exceptions.
	// Other context groups and control values are rejected, not normalized.
	return selected.MxCsr == mxcsr && (mask ? !(mxcsr & ~mask) : mxcsr == current.FltSave.MxCsr) &&
		   (selected.FltSave.ControlWord & kX87ExceptionMasks) == kX87ExceptionMasks &&
		   (mxcsr & kSseExceptionMasks) == kSseExceptionMasks;
}

DWORD fail(ContextRestorePreparation64 &output, ContextRestoreKind64 kind, DWORD status = kNotSupported) {
	output.kind = kind;
	output.status = status;
	return static_cast<DWORD>(kind);
}
} // namespace

static_assert(std::is_trivially_copyable_v<ContextRestorePreparation64>);
static_assert(std::is_trivially_destructible_v<ContextRestorePreparation64>);

DWORD wiboPrepareContextRestore64(const SoftwareExceptionCapture64 *entry, ContextRestorePreparation64 *output) {
	*output = {};
	if (!entry)
		return fail(*output, ContextRestoreKind64::InvalidContext, kInvalidParameter);
	if (wibo::hasActiveVectoredExceptionTraversal())
		return fail(*output, ContextRestoreKind64::UnsupportedActivationTransfer);
	const ULONGLONG source = entry->context.Rcx;
	if (!accessibleRange(source, sizeof(CONTEXT64), false))
		return fail(*output, ContextRestoreKind64::InvalidContext, kInvalidParameter);
	std::memcpy(&output->context, reinterpret_cast<const void *>(source), sizeof(CONTEXT64));
	if (entry->context.Rdx) {
		if (!accessibleRange(entry->context.Rdx, sizeof(EXCEPTION_RECORD), false))
			return fail(*output, ContextRestoreKind64::UnsupportedExceptionRecord);
		auto *record = reinterpret_cast<EXCEPTION_RECORD *>(entry->context.Rdx);
		if (record->ExceptionCode != kConsolidate || record->NumberParameters < 1 || record->NumberParameters > 15 ||
			(record->ExceptionFlags & kCollidedUnwind) || !executableTarget(record->ExceptionInformation[0]) ||
			!accessibleRange(source, sizeof(CONTEXT64), true))
			return fail(*output, ContextRestoreKind64::UnsupportedExceptionRecord);
		output->record = record;
		output->sourceContext = reinterpret_cast<CONTEXT64 *>(source);
		output->callback = reinterpret_cast<PVOID>(record->ExceptionInformation[0]);
	}
	const auto &selected = output->context;
	if (!supportedLegacyState(selected, entry->context))
		return fail(*output, ContextRestoreKind64::UnsupportedContextState);
#if defined(__APPLE__)
	TEB *teb = currentTebForGuestTransition();
#else
	TEB *teb = currentThreadTeb;
#endif
	if (!teb || teb->Tib.StackLimit >= teb->Tib.StackBase || selected.Rsp < entry->context.Rsp ||
		selected.Rsp < sizeof(ULONGLONG) || selected.Rsp - sizeof(ULONGLONG) < teb->Tib.StackLimit ||
		selected.Rsp > teb->Tib.StackBase || (selected.Rsp & 7) ||
		!accessibleRange(selected.Rsp - sizeof(ULONGLONG), sizeof(ULONGLONG), true) || !executableTarget(selected.Rip))
		return fail(*output, ContextRestoreKind64::UnsupportedControlTransfer);
	// All range queries have returned and released their ownership scopes.
	// The required gate validates the full activation chain before unlinking it.
	if (output->record) {
		const auto stackLow = reinterpret_cast<ULONGLONG>(output) - WIBO_CONTEXT_RESTORE_FRAME_OUTPUT;
		if (!wiboValidateContextRestoreTransfer64(selected.Rsp) ||
			!wibo::linkSoftwareExceptionActivation(&output->activation, &output->context, stackLow, entry->context.Rsp,
												   SoftwareExceptionActivationPhase64::Consolidation))
			return fail(*output, ContextRestoreKind64::UnsupportedActivationTransfer);
		return fail(*output, ContextRestoreKind64::Consolidate, 0);
	}
	if (!wiboPrepareContextRestoreTransfer64(selected.Rsp))
		return fail(*output, ContextRestoreKind64::UnsupportedActivationTransfer);
	output->kind = ContextRestoreKind64::Restore;
	output->status = 0;
	return 0;
}

DWORD wiboCompleteContextConsolidation64(ContextRestorePreparation64 *output) {
	if (!output->sourceContext || !wibo::unlinkSoftwareExceptionActivation(&output->activation))
		return fail(*output, ContextRestoreKind64::UnsupportedActivationTransfer);
	if (!executableTarget(output->context.Rip) ||
		!accessibleRange(reinterpret_cast<ULONGLONG>(output->sourceContext), sizeof(CONTEXT64), true))
		return fail(*output, ContextRestoreKind64::UnsupportedControlTransfer);
	// The callback may select a continuation, but mutation of other selected
	// context fields requires a separate restoration contract.
	CONTEXT64 original{};
	std::memcpy(&original, output->sourceContext, sizeof(original));
	original.Rip = output->context.Rip;
	if (std::memcmp(&original, &output->context, sizeof(original)) != 0)
		return fail(*output, ContextRestoreKind64::UnsupportedContextState);
	if (!wiboPrepareContextRestoreTransfer64(output->context.Rsp))
		return fail(*output, ContextRestoreKind64::UnsupportedActivationTransfer);
	output->sourceContext->Rip = output->context.Rip;
	output->kind = ContextRestoreKind64::Restore;
	output->status = 0;
	return 0;
}

[[noreturn]] void wiboUnsupportedContextRestore64(const ContextRestorePreparation64 *output) {
	std::fprintf(stderr, "Unsupported context restoration: kind=%u status=0x%08x\n",
				 static_cast<unsigned>(output->kind), output->status);
	kernel32::exitInternal(output->status);
}
#endif
