#include "contextapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

#include <cstddef>
#include <cstdint>

namespace {
constexpr DWORD kContextXState = 0x40;
#ifndef WIBO_GUEST_64
constexpr DWORD kContextExtendedRegisters = 0x20;
#endif
constexpr DWORD kSupportedContextBits = CONTEXT_ARCH | 0x7f | 0xd8000000;

struct ContextChunk {
	int32_t offset;
	DWORD length;
};

struct ContextEx {
	ContextChunk all;
	ContextChunk legacy;
	ContextChunk xstate;
};

static_assert(sizeof(ContextEx) == 24);

bool validContext(const CONTEXT *context) { return context && (context->ContextFlags & CONTEXT_ARCH) == CONTEXT_ARCH; }
} // namespace

namespace kernel32 {

BOOL WINAPI InitializeContext(PVOID buffer, DWORD contextFlags, GUEST_PTR *context, PDWORD contextLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeContext(%p, 0x%x, %p, %p)\n", buffer, contextFlags, context, contextLength);
	if (!contextLength || (contextFlags & CONTEXT_ARCH) != CONTEXT_ARCH || (contextFlags & ~kSupportedContextBits)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD inputLength = *contextLength;
	// Extended guest state is unavailable; preserve the requested legacy sections.
	contextFlags &= ~kContextXState;
#ifdef WIBO_GUEST_64
	constexpr DWORD kContextExSize = 32;
	constexpr DWORD kAlignmentAllowance = 7;
	constexpr DWORD kContextAlignment = 16;
	constexpr DWORD kLegacyLength = sizeof(CONTEXT);
#else
	constexpr DWORD kContextExSize = 24;
	constexpr DWORD kAlignmentAllowance = 3;
	constexpr DWORD kContextAlignment = 4;
	constexpr DWORD kLegacyLength = offsetof(CONTEXT, ExtendedRegisters);
#endif
	DWORD requiredLength = sizeof(CONTEXT) + kContextExSize + kAlignmentAllowance;
	if (buffer) {
		const uintptr_t address = reinterpret_cast<uintptr_t>(buffer);
		const DWORD alignmentOffset = static_cast<DWORD>((-address) & (kContextAlignment - 1));
		const DWORD alignedLength = alignmentOffset + sizeof(CONTEXT) + kContextExSize;
		if (alignedLength > requiredLength)
			requiredLength = alignedLength;
	}
	*contextLength = requiredLength;
	if (!buffer || inputLength < requiredLength) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (!context) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const uintptr_t alignedAddress =
		(reinterpret_cast<uintptr_t>(buffer) + kContextAlignment - 1) & ~(uintptr_t)(kContextAlignment - 1);
	auto *initialized = reinterpret_cast<CONTEXT *>(alignedAddress);
	initialized->ContextFlags = contextFlags;
	auto *extension = reinterpret_cast<ContextEx *>(initialized + 1);
	extension->all = {-static_cast<int32_t>(sizeof(CONTEXT)), sizeof(CONTEXT) + sizeof(ContextEx)};
	extension->legacy = {-static_cast<int32_t>(sizeof(CONTEXT)),
						 (contextFlags & 0x20) ? static_cast<DWORD>(sizeof(CONTEXT)) : kLegacyLength};
	extension->xstate = {25, 0};
	*context = toGuestPtr(initialized);
	return TRUE;
}

BOOL WINAPI GetXStateFeaturesMask(const CONTEXT *context, ULONGLONG *featureMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetXStateFeaturesMask(%p, %p)\n", context, featureMask);
	if (!validContext(context) || !featureMask)
		return FALSE;
#ifdef WIBO_GUEST_64
	*featureMask = (context->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT ? 3 : 0;
#else
	*featureMask = (context->ContextFlags & kContextExtendedRegisters) ? 3 : 0;
#endif
	return TRUE;
}

BOOL WINAPI SetXStateFeaturesMask(CONTEXT *context, ULONGLONG featureMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetXStateFeaturesMask(%p, 0x%llx)\n", context, static_cast<unsigned long long>(featureMask));
	if (!validContext(context))
		return FALSE;
	if (featureMask & 3) {
#ifdef WIBO_GUEST_64
		context->ContextFlags |= CONTEXT_FLOATING_POINT;
#else
		context->ContextFlags |= CONTEXT_ARCH | kContextExtendedRegisters;
#endif
	}
	return (featureMask & ~ULONGLONG{3}) == 0;
}

PVOID WINAPI LocateXStateFeature(CONTEXT *context, DWORD featureId, PDWORD length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LocateXStateFeature(%p, %u, %p)\n", context, featureId, length);
	if (!validContext(context))
		return nullptr;
	if (featureId == 0) {
		if (length)
			*length = offsetof(XMM_SAVE_AREA32, XmmRegisters);
#ifdef WIBO_GUEST_64
		return &context->FltSave;
#else
		return context->ExtendedRegisters;
#endif
	}
	if (featureId == 1) {
		if (length)
#ifdef WIBO_GUEST_64
			*length = sizeof(M128A) * 16;
#else
			*length = sizeof(M128A) * 8;
#endif
#ifdef WIBO_GUEST_64
		return context->FltSave.XmmRegisters;
#else
		return context->ExtendedRegisters + offsetof(XMM_SAVE_AREA32, XmmRegisters);
#endif
	}
	return nullptr;
}

} // namespace kernel32
