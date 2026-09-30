#include "software_exception_x86.h"

#ifndef WIBO_GUEST_64
#include "common.h"
#include "entry.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "kernel32/vectored_exception.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace {
constexpr DWORD kBadStack = 0xc0000028, kInvalidDisposition = 0xc0000026;
constexpr DWORD kNoncontinuable = 0xc0000025, kUnwind = 0xc0000027;
constexpr DWORD kInvalidUnwindTarget = 0xc0000029, kUnsupported = 0xc00000bb;
constexpr GUEST_PTR kChainEnd = 0xffffffff;
constexpr DWORD kFrameLimit = 1024;

[[noreturn]] void fail(DWORD code, const char *reason) {
	DEBUG_LOG("x86 software exception: %s (0x%x)\n", reason, code);
	kernel32::exitInternal(code);
}

bool memory(GUEST_PTR address, size_t size, bool writable = false, bool executable = false) {
	if (size > UINT32_MAX - address)
		return false;
	while (size) {
		MEMORY_BASIC_INFORMATION region{};
		if (wibo::heap::virtualQuery(fromGuestPtr(address), &region) != wibo::heap::VmStatus::Success ||
			region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
			address < region.BaseAddress || address - region.BaseAddress >= region.RegionSize)
			return false;
		const DWORD protection = region.Protect & 0xff;
		if (executable && protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
			protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
			return false;
		if (!executable && protection == PAGE_EXECUTE)
			return false;
		if (writable && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
			protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
			return false;
		const auto amount = std::min<size_t>(size, region.RegionSize - (address - region.BaseAddress));
		size -= amount;
		address += static_cast<GUEST_PTR>(amount);
	}
	return true;
}

bool stack(GUEST_PTR address, size_t size) {
	const auto *teb = currentThreadTeb;
	return teb && !(address & 3) && address >= teb->Tib.StackLimit && address < teb->Tib.StackBase &&
		size <= teb->Tib.StackBase - address && memory(address, size, true);
}

SoftwareExceptionFrame32 &frame(GUEST_PTR address) {
	if ((address & 15) || !stack(address, sizeof(SoftwareExceptionFrame32)))
		fail(kBadStack, "invalid dispatch storage");
	return *fromGuestPtr<SoftwareExceptionFrame32>(address);
}

EXCEPTION_RECORD &record(SoftwareExceptionFrame32 &state) {
	if (!memory(state.recordPointer, sizeof(EXCEPTION_RECORD), true))
		fail(kBadStack, "invalid exception record");
	return *fromGuestPtr<EXCEPTION_RECORD>(state.recordPointer);
}

SoftwareExceptionGuard32 registration(GUEST_PTR address) {
	if (!stack(address, 8))
		fail(kBadStack, "invalid registration frame");
	SoftwareExceptionGuard32 node{};
	std::memcpy(&node, fromGuestPtr(address), 8);
	if (node.next != kChainEnd && (!stack(node.next, 8) || node.next <= address))
		fail(kBadStack, "registration chain does not advance");
	if (node.handler != toGuestPtr(reinterpret_cast<void *>(&wiboExceptionGuard32)) &&
		!memory(node.handler, 1, false, true))
		fail(kBadStack, "registration handler is not executable");
	return node;
}

void validateResume(SoftwareExceptionFrame32 &state) {
	auto &context = state.context;
	if ((context.ContextFlags & 0x10007) != 0x10007 || (context.ContextFlags & ~DWORD{0x1002f}) ||
		!stack(context.Esp - 4, 4) || !memory(context.Eip, 1, false, true))
		fail(kBadStack, "unsupported continuation context");
	const DWORD segments[] = {context.SegCs, context.SegSs, context.SegDs, context.SegEs, context.SegFs, context.SegGs};
	if (std::memcmp(segments, state.segments, sizeof(segments)) != 0)
		fail(kUnsupported, "segment-selector changes are unsupported");
	DWORD mxcsr = 0;
	std::memcpy(&mxcsr, context.ExtendedRegisters + 24, 4);
	if (context.ContextFlags & 0x20) {
		// Architectural fallback when FXSAVE reports a zero feature mask.
		if (mxcsr & ~(state.capturedMxcsrMask ? state.capturedMxcsrMask : 0xffbfU))
			fail(kBadStack, "invalid MXCSR continuation");
	}
}

DWORD next(SoftwareExceptionFrame32 &state) {
	if (++state.visited > kFrameLimit)
		fail(kBadStack, "registration traversal limit exceeded");
	if (state.phase && state.current == state.target && state.target) {
		validateResume(state);
		return 1;
	}
	if (state.current == kChainEnd) {
		if (state.phase)
			fail(state.target ? kInvalidUnwindTarget : record(state).ExceptionCode, "unwind target absent");
		// Retain the pre-existing debugger thread-name notification behavior only
		// after frame handlers have had their chance. This is not an SEH success.
		if (record(state).ExceptionCode == 0x406d1388 && !(record(state).ExceptionFlags & 1)) {
			validateResume(state);
			return 1;
		}
		const auto filter = kernel32::currentUnhandledExceptionFilter();
		if (!filter)
			fail(record(state).ExceptionCode, "unhandled software exception");
		state.handlerKind = 1;
		state.handler = toGuestPtr(reinterpret_cast<void *>(filter));
		if (!memory(state.handler, 1, false, true))
			fail(kBadStack, "top-level filter is not executable");
		state.pointers = {state.recordPointer, toGuestPtr(&state.context)};
		return 0;
	}
	const auto node = registration(state.current);
	state.handlerKind = 0;
	state.handler = node.handler;
	state.dispatcher = 0;
	state.guard = {currentThreadTeb->Tib.ExceptionList, toGuestPtr(reinterpret_cast<void *>(&wiboExceptionGuard32)),
				   state.current, state.phase};
	if (state.guard.next != kChainEnd && state.guard.next <= toGuestPtr(&state.guard))
		fail(kBadStack, "protector cannot precede registration chain");
	currentThreadTeb->Tib.ExceptionList = toGuestPtr(&state.guard);
	return 0;
}

DWORD resumeOrSecondary(SoftwareExceptionFrame32 &state) {
	auto &exception = record(state);
	if (exception.ExceptionFlags & 1) {
		if (++state.restarts > 1)
			fail(kNoncontinuable, "repeated noncontinuable continuation");
		state.nestedRecord = {};
		state.nestedRecord.ExceptionCode = kNoncontinuable;
		state.nestedRecord.ExceptionFlags = 1;
		state.nestedRecord.ExceptionRecord = state.recordPointer;
		state.nestedRecord.ExceptionAddress = state.context.Eip;
		state.recordPointer = toGuestPtr(&state.nestedRecord);
		state.handlerKind = 0;
		state.current = currentThreadTeb->Tib.ExceptionList;
		return next(state);
	}
	validateResume(state);
	return 1;
}
} // namespace

static_assert(std::is_trivially_destructible_v<SoftwareExceptionFrame32>);

namespace entry {
DWORD CDECL prepareSoftwareException32(GUEST_PTR address, DWORD mode) {
	auto &state = frame(address);
	if (mode > 2 || !stack(state.savedStack, mode == 1 ? 44 : 56))
		fail(kBadStack, "invalid captured entry stack");
	const auto *arguments = fromGuestPtr<const DWORD>(state.savedStack + 40);
	state.segments[0] = state.context.SegCs;
	state.segments[1] = state.context.SegSs;
	state.segments[2] = state.context.SegDs;
	state.segments[3] = state.context.SegEs;
	state.segments[4] = state.context.SegFs;
	state.segments[5] = state.context.SegGs;
	std::memcpy(&state.capturedMxcsrMask, state.context.ExtendedRegisters + 28, sizeof(DWORD));
	state.recordPointer = toGuestPtr(&state.record);
	state.current = currentThreadTeb->Tib.ExceptionList;
	state.phase = mode == 2;
	if (mode == 0) {
		state.record.ExceptionCode = arguments[0];
		state.record.ExceptionFlags = arguments[1] & 1;
		state.record.ExceptionAddress = toGuestPtr(reinterpret_cast<void *>(&wiboRaiseException32));
		if (arguments[3]) {
			state.record.NumberParameters = std::min(arguments[2], EXCEPTION_MAXIMUM_PARAMETERS);
			if (!memory(arguments[3], state.record.NumberParameters * sizeof(DWORD)))
				fail(kBadStack, "invalid software exception arguments");
			std::memcpy(state.record.ExceptionInformation, fromGuestPtr(arguments[3]),
						state.record.NumberParameters * sizeof(DWORD));
		}
	} else if (mode == 1) {
		state.recordPointer = arguments[0];
		record(state).ExceptionAddress = state.context.Eip;
		if (record(state).NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS)
			fail(kBadStack, "invalid RtlRaiseException parameter count");
	} else {
		state.target = arguments[0];
		state.targetIp = arguments[1];
		state.returnValue = arguments[3];
		if (arguments[2])
			state.recordPointer = arguments[2];
		else {
			state.record.ExceptionCode = kUnwind;
			state.record.ExceptionAddress = state.context.Eip;
		}
		record(state).ExceptionFlags |= state.target ? 2 : 6;
		state.context.Eax = state.returnValue;
		// x86 CRT callers commonly pass the post-call continuation itself. A
		// different target IP needs independently verified x86 stack semantics.
		if (state.target && state.targetIp && state.targetIp != state.context.Eip)
			fail(kUnsupported, "distinct x86 unwind target IP is unsupported");
		if (state.target && state.target != kChainEnd && !stack(state.target, 8))
			fail(kInvalidUnwindTarget, "invalid unwind target");
	}
	if (wibo::hasRegisteredVectoredExceptionHandlers())
		fail(kUnsupported, "x86 vectored software dispatch is not yet implemented");
	DEBUG_LOG("x86 software exception: mode=%u code=0x%x flags=0x%x head=0x%x\n", mode,
			  record(state).ExceptionCode, record(state).ExceptionFlags, state.current);
	return next(state);
}

DWORD CDECL finishSoftwareExceptionHandler32(GUEST_PTR address, LONG disposition) {
	auto &state = frame(address);
	auto &exception = record(state);
	if (state.handlerKind) {
		if (disposition == -1)
			return resumeOrSecondary(state);
		fail(exception.ExceptionCode, "top-level filter did not continue execution");
	}
	if (currentThreadTeb->Tib.ExceptionList != toGuestPtr(&state.guard))
		fail(kBadStack, "returning personality abandoned its protector");
	currentThreadTeb->Tib.ExceptionList = state.guard.next;
	DEBUG_LOG("x86 software handler: frame=0x%x disposition=%d dispatcher=0x%x\n", state.current, disposition,
			  state.dispatcher);
	const auto node = registration(state.current);
	if (!state.phase) {
		if (disposition == 0)
			return resumeOrSecondary(state);
		if (disposition == 2) {
			if (state.dispatcher < state.current || !stack(state.dispatcher, 8))
				fail(kInvalidDisposition, "invalid nested exception boundary");
			state.nestedFloor = std::max(state.nestedFloor, state.dispatcher);
			exception.ExceptionFlags |= 0x10;
		} else if (disposition != 1)
			fail(kInvalidDisposition, "invalid search disposition");
		if (state.current == state.nestedFloor) {
			state.nestedFloor = 0;
			exception.ExceptionFlags &= ~DWORD{0x10};
		}
		state.current = node.next;
	} else {
		if (disposition == 3) {
			if (state.dispatcher < state.current || (state.target && state.dispatcher >= state.target))
				fail(kInvalidDisposition, "invalid collided unwind boundary");
			// Verify the collided target belongs to the actual ascending chain.
			GUEST_PTR cursor = state.current;
			for (DWORD count = 0; cursor != state.dispatcher; ++count) {
				if (cursor == kChainEnd || count == kFrameLimit)
					fail(kInvalidDisposition, "collided unwind frame absent");
				cursor = registration(cursor).next;
			}
			state.current = registration(cursor).next;
			exception.ExceptionFlags |= 0x40;
		} else if (disposition == 1) {
			state.current = node.next;
			exception.ExceptionFlags &= ~DWORD{0x40};
		} else
			fail(kInvalidDisposition, "invalid unwind disposition");
		currentThreadTeb->Tib.ExceptionList = state.current;
		if (state.target && state.current != kChainEnd && state.current > state.target)
			fail(kInvalidUnwindTarget, "unwind passed target");
	}
	return next(state);
}

void CDECL abortSoftwareException32(DWORD code) { fail(code, "guest dispatch storage unavailable"); }
} // namespace entry
#endif
