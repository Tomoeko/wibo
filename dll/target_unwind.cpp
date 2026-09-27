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

bool writableRecord(ULONGLONG address) {
	if (!address || sizeof(EXCEPTION_RECORD) > std::numeric_limits<ULONGLONG>::max() - address)
		return false;
	size_t remaining = sizeof(EXCEPTION_RECORD);
	while (remaining) {
		MEMORY_BASIC_INFORMATION region{};
		if (wibo::heap::virtualQuery(reinterpret_cast<const void *>(address), &region) !=
				wibo::heap::VmStatus::Success ||
			region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			address < region.BaseAddress || address - region.BaseAddress >= region.RegionSize)
			return false;
		const DWORD protection = region.Protect & 0xff;
		if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READWRITE &&
			protection != PAGE_EXECUTE_WRITECOPY)
			return false;
		const auto available = region.RegionSize - (address - region.BaseAddress);
		const auto amount = std::min<size_t>(remaining, available);
		remaining -= amount;
		address += amount;
	}
	return true;
}

DWORD unsupported(TargetUnwindPreparation64 &output, const char *reason) {
	DEBUG_LOG("target unwind: unsupported %s\n", reason);
	output.kind = TargetUnwindKind64::Unsupported;
	output.status = kNotSupported;
	if (wibo::currentSoftwareExceptionActivation() == &output.frames.activation)
		wibo::unlinkSoftwareExceptionActivation(&output.frames.activation);
	return static_cast<DWORD>(output.kind);
}
} // namespace

static_assert(std::is_trivially_copyable_v<TargetUnwindPreparation64>);
static_assert(std::is_trivially_destructible_v<TargetUnwindPreparation64>);

DWORD wiboPrepareTargetUnwind64(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output) {
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
	if (entry->context.R8) {
		if (!writableRecord(entry->context.R8))
			return unsupported(*output, "unavailable exception record storage");
		output->record = reinterpret_cast<EXCEPTION_RECORD *>(entry->context.R8);
	} else {
		output->localRecord.ExceptionCode = kUnwindCode;
		output->localRecord.ExceptionAddress = entry->context.Rip;
		output->record = &output->localRecord;
	}
	if (output->record->ExceptionCode == kLongJumpCode || output->record->ExceptionCode == kUnwindConsolidateCode ||
		(output->record->ExceptionFlags & kCollidedUnwind))
		return unsupported(*output, "special restoration record or collided unwind");
	output->record->ExceptionFlags |= kUnwinding;
	frames.walkingContext = entry->context;
	const auto stackLow = reinterpret_cast<ULONGLONG>(output) - WIBO_TARGET_UNWIND_FRAME_OUTPUT;
	if (!wibo::linkSoftwareExceptionActivation(&frames.activation, &entry->context, stackLow, entry->context.Rsp,
											   SoftwareExceptionActivationPhase64::TargetUnwind))
		return unsupported(*output, "activation ancestry");
	for (; frames.frameCount < kFrameLimit; ++frames.frameCount) {
		const auto bridge = wibo::restartSoftwareExceptionAtBridge(frames.walkingContext, output->targetFrame);
		if (bridge == wibo::SoftwareExceptionBridgeResult64::Unsupported)
			return unsupported(*output, "native callback boundary");
		if (bridge == wibo::SoftwareExceptionBridgeResult64::Restarted)
			DEBUG_LOG("target unwind: restarted guest walk rip=%llx rsp=%llx\n", frames.walkingContext.Rip,
					  frames.walkingContext.Rsp);
		if (!wibo::prepareSoftwareExceptionFrame64(frames, kUnwindHandler))
			return unsupported(*output, "frame preparation");
		auto &dispatcher = frames.dispatcher;
		if (dispatcher.EstablisherFrame > output->targetFrame)
			return unsupported(*output, "target frame not found");
		const bool target = dispatcher.EstablisherFrame == output->targetFrame;
		dispatcher.ContextRecord = &frames.frameContext;
		dispatcher.TargetIp = output->targetIp;
		if (target)
			output->record->ExceptionFlags |= kTargetUnwind;
		if (dispatcher.LanguageHandler) {
			const auto expected = dispatcher;
			const LONG result = wibo::invokeSoftwareExceptionFrameHandler64(
				dispatcher.LanguageHandler, output->record, dispatcher.EstablisherFrame, &frames.frameContext,
				&dispatcher, &frames.activation);
			DEBUG_LOG("target unwind handler: pc=%llx frame=%llx flags=%x result=%d\n", expected.ControlPc,
					  expected.EstablisherFrame, output->record->ExceptionFlags, result);
			if (result != kContinueSearch || std::memcmp(&dispatcher, &expected, sizeof(dispatcher)) != 0)
				return unsupported(*output, "handler disposition or dispatcher mutation");
		}
		if (target) {
			output->resumeContext = frames.frameContext;
			output->resumeContext.Rax = output->returnValue;
			output->resumeContext.Rip = output->targetIp;
			output->kind = TargetUnwindKind64::Restore;
			output->status = 0;
			return 0;
		}
		if (frames.walkingContext.Rsp == frames.stackBase)
			return unsupported(*output, "end of stack before target");
	}
	return unsupported(*output, "frame limit");
}

[[noreturn]] void wiboUnsupportedTargetUnwind64(const TargetUnwindPreparation64 *output) {
	std::fprintf(stderr, "Unsupported target unwind: kind=%u status=0x%08x\n", static_cast<unsigned>(output->kind),
				 output->status);
	kernel32::exitInternal(output->status ? output->status : kNotSupported);
}
#endif
