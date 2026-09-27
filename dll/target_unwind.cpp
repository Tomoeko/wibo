#include "target_unwind.h"

#ifdef WIBO_GUEST_64
#include "common.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "setup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
constexpr DWORD kNotSupported = 0xc00000bb;
constexpr DWORD kUnwindCode = 0xc0000027;
constexpr DWORD kLongJumpCode = 0x80000026, kUnwindConsolidateCode = 0x80000029;
constexpr DWORD kUnwinding = 2, kTargetUnwind = 0x20;
constexpr DWORD kCollidedUnwind = 0x40;
constexpr DWORD kUnwindHandler = 2, kFrameLimit = 1024;
constexpr LONG kContinueSearch = 1;

bool accessibleRange(ULONGLONG address, size_t length, bool writable) {
	if (!address || !length || length > std::numeric_limits<ULONGLONG>::max() - address)
		return false;
	size_t remaining = length;
	while (remaining) {
		MEMORY_BASIC_INFORMATION region{};
		if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) !=
				wibo::heap::VmStatus::Success ||
			region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			address < region.BaseAddress || address - region.BaseAddress >= region.RegionSize)
			return false;
		const DWORD protection = region.Protect & 0xff;
		const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
							  protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
		const bool canRead = canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
		if (writable ? !canWrite : !canRead)
			return false;
		const auto available = region.RegionSize - (address - region.BaseAddress);
		const auto amount = std::min<size_t>(remaining, available);
		remaining -= amount;
		address += amount;
	}
	return true;
}

struct UnwindHistoryEntry64 {
	ULONGLONG imageBase;
	RUNTIME_FUNCTION *function;
};

struct UnwindHistoryTable64 {
	DWORD count;
	BYTE localHint, globalHint, search, once;
	ULONGLONG lowAddress, highAddress;
	UnwindHistoryEntry64 entries[12];
};

static_assert(sizeof(UnwindHistoryTable64) == 216 && alignof(UnwindHistoryTable64) == 8);
static_assert(offsetof(UnwindHistoryTable64, lowAddress) == 8);
static_assert(offsetof(UnwindHistoryTable64, highAddress) == 16);
static_assert(offsetof(UnwindHistoryTable64, entries) == 24);

void mergeCapturedContext(CONTEXT64 &destination, const CONTEXT64 &capture) {
	// Capture only the selected groups. Home, debug, vector and reserved FP
	// storage belongs to the caller and is not initialized by this capture.
	auto *bytes = reinterpret_cast<BYTE *>(&destination);
	const auto *source = reinterpret_cast<const BYTE *>(&capture);
	constexpr size_t controlStart = offsetof(CONTEXT64, ContextFlags);
	constexpr size_t controlSize = offsetof(CONTEXT64, Dr0) - controlStart;
	constexpr size_t integerStart = offsetof(CONTEXT64, Rax);
	constexpr size_t integerSize = offsetof(CONTEXT64, FltSave) - integerStart;
	std::memcpy(bytes + controlStart, source + controlStart, controlSize);
	std::memcpy(bytes + integerStart, source + integerStart, integerSize);
	std::memcpy(&destination.FltSave, &capture.FltSave, offsetof(XMM_SAVE_AREA32, Reserved4));
}

DWORD unsupported(TargetUnwindPreparation64 &output, const char *reason) {
	DEBUG_LOG("target unwind: unsupported %s\n", reason);
	output.kind = TargetUnwindKind64::Unsupported;
	output.status = kNotSupported;
	if (wibo::currentSoftwareExceptionActivation() == &output.frames.activation)
		wibo::unlinkSoftwareExceptionActivation(&output.frames.activation);
	return static_cast<DWORD>(output.kind);
}

DWORD prepareTargetUnwind(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output, bool extended) {
	*output = {};
	output->kind = TargetUnwindKind64::Unsupported;
	output->status = kNotSupported;
#if defined(__APPLE__)
	const TEB *teb = currentTebForGuestTransition();
#else
	const TEB *teb = currentThreadTeb;
#endif
	if (!entry || !teb)
		return unsupported(*output, "missing captured thread context");
	output->targetFrame = entry->context.Rcx;
	output->targetIp = entry->context.Rdx;
	output->returnValue = entry->context.R9;
	auto &frames = output->frames;
	frames.stackLimit = teb->Tib.StackLimit;
	frames.stackBase = teb->Tib.StackBase;
	if (!output->targetFrame || frames.stackLimit >= frames.stackBase || (output->targetFrame & 7) ||
		output->targetFrame < entry->context.Rsp || output->targetFrame < frames.stackLimit ||
		output->targetFrame >= frames.stackBase)
		return unsupported(*output, "target frame or exit unwind");
	const auto stackLow = reinterpret_cast<ULONGLONG>(output) - WIBO_TARGET_UNWIND_FRAME_OUTPUT;
	CONTEXT64 *callerContext = nullptr;
	PVOID historyTable = nullptr;
	if (extended) {
		const ULONGLONG callerRsp = entry->context.Rsp;
		if (callerRsp < frames.stackLimit || callerRsp >= frames.stackBase || 48 > frames.stackBase - callerRsp ||
			!accessibleRange(callerRsp + 32, 16, false))
			return unsupported(*output, "unavailable stack arguments");
		ULONGLONG arguments[2];
		std::memcpy(arguments, reinterpret_cast<const void *>(callerRsp + 32), sizeof(arguments));
		const auto contextAddress = arguments[0];
		if ((contextAddress & 15) || !accessibleRange(contextAddress, sizeof(CONTEXT64), true) ||
			(contextAddress < callerRsp && contextAddress + sizeof(CONTEXT64) > stackLow))
			return unsupported(*output, "unavailable or overlapping caller context");
		if (arguments[1] && ((arguments[1] & 7) || !accessibleRange(arguments[1], sizeof(UnwindHistoryTable64), true)))
			return unsupported(*output, "unavailable unwind history table");
		callerContext = reinterpret_cast<CONTEXT64 *>(contextAddress);
		historyTable = reinterpret_cast<PVOID>(arguments[1]);
	}
	if (entry->context.R8) {
		if (!accessibleRange(entry->context.R8, sizeof(EXCEPTION_RECORD), true))
			return unsupported(*output, "unavailable exception record storage");
		output->record = reinterpret_cast<EXCEPTION_RECORD *>(entry->context.R8);
	} else {
		output->localRecord.ExceptionCode = kUnwindCode;
		output->localRecord.ExceptionAddress = entry->context.Rip;
		output->record = &output->localRecord;
	}
	DEBUG_LOG("target unwind: extended=%u target=%llx ip=%llx record=%p code=%x flags=%x context=%p history=%p\n",
			  extended, output->targetFrame, output->targetIp, output->record, output->record->ExceptionCode,
			  output->record->ExceptionFlags, callerContext, historyTable);
	if (output->record->ExceptionCode == kLongJumpCode || (output->record->ExceptionFlags & kCollidedUnwind))
		return unsupported(*output, "special restoration record or collided unwind");
	output->record->ExceptionFlags |= kUnwinding;
	if (callerContext) {
		mergeCapturedContext(*callerContext, entry->context);
		frames.walkingContext = *callerContext;
	} else {
		frames.walkingContext = entry->context;
	}
	if (!wibo::linkSoftwareExceptionActivation(&frames.activation, &entry->context, stackLow, entry->context.Rsp,
											   SoftwareExceptionActivationPhase64::TargetUnwind))
		return unsupported(*output, "activation ancestry");
	for (; frames.frameCount < kFrameLimit; ++frames.frameCount) {
		const auto bridge = wibo::restartSoftwareExceptionAtBridge(frames.walkingContext, output->targetFrame);
		if (bridge == wibo::SoftwareExceptionBridgeResult64::Unsupported)
			return unsupported(*output, "native callback boundary");
		if (bridge == wibo::SoftwareExceptionBridgeResult64::Restarted) {
			if (callerContext) {
				// The saved origin supplies machine state, while unselected
				// context storage remains owned by the caller across this bridge.
				mergeCapturedContext(*callerContext, frames.walkingContext);
				frames.walkingContext = *callerContext;
			}
			DEBUG_LOG("target unwind: restarted guest walk rip=%llx rsp=%llx\n", frames.walkingContext.Rip,
					  frames.walkingContext.Rsp);
		}
		if (!wibo::prepareSoftwareExceptionFrame64(frames, kUnwindHandler, historyTable))
			return unsupported(*output, "frame preparation");
		auto &dispatcher = frames.dispatcher;
		if (dispatcher.EstablisherFrame > output->targetFrame)
			return unsupported(*output, "target frame not found");
		const bool target = dispatcher.EstablisherFrame == output->targetFrame;
		CONTEXT64 *context = callerContext ? callerContext : &frames.frameContext;
		if (callerContext)
			*callerContext = frames.frameContext;
		dispatcher.ContextRecord = context;
		dispatcher.TargetIp = output->targetIp;
		if (target)
			output->record->ExceptionFlags |= kTargetUnwind;
		if (dispatcher.LanguageHandler) {
			const auto expected = dispatcher;
			const LONG result = wibo::invokeSoftwareExceptionFrameHandler64(dispatcher.LanguageHandler, output->record,
																			dispatcher.EstablisherFrame, context,
																			&dispatcher, &frames.activation);
			DEBUG_LOG("target unwind handler: pc=%llx frame=%llx flags=%x result=%d\n", expected.ControlPc,
					  expected.EstablisherFrame, output->record->ExceptionFlags, result);
			if (result != kContinueSearch || std::memcmp(&dispatcher, &expected, sizeof(dispatcher)) != 0)
				return unsupported(*output, "handler disposition or dispatcher mutation");
		}
		if (target) {
			output->resumeContext = *context;
			output->resumeContext.Rax = output->returnValue;
			if (output->record->ExceptionCode != kUnwindConsolidateCode)
				output->resumeContext.Rip = output->targetIp;
			if (callerContext)
				*callerContext = output->resumeContext;
			output->restoreContext = callerContext ? callerContext : &output->resumeContext;
			output->kind = TargetUnwindKind64::Restore;
			output->status = 0;
			return 0;
		}
		if (callerContext)
			*callerContext = frames.walkingContext;
		if (frames.walkingContext.Rsp == frames.stackBase)
			return unsupported(*output, "end of stack before target");
	}
	return unsupported(*output, "frame limit");
}
} // namespace

static_assert(std::is_trivially_copyable_v<TargetUnwindPreparation64>);
static_assert(std::is_trivially_destructible_v<TargetUnwindPreparation64>);
static_assert(offsetof(TargetUnwindPreparation64, record) == WIBO_TARGET_UNWIND_RECORD_OFFSET);

DWORD wiboPrepareTargetUnwind64(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output) {
	return prepareTargetUnwind(entry, output, false);
}

DWORD wiboPrepareTargetUnwindEx64(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output) {
	return prepareTargetUnwind(entry, output, true);
}

[[noreturn]] void wiboUnsupportedTargetUnwind64(const TargetUnwindPreparation64 *output) {
	std::fprintf(stderr, "Unsupported target unwind: kind=%u status=0x%08x\n", static_cast<unsigned>(output->kind),
				 output->status);
	kernel32::exitInternal(output->status ? output->status : kNotSupported);
}
#endif
