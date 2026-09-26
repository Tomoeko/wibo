#include "oleaut32.h"

#include "common.h"
#include "context.h"
#include "heap.h"
#include "modules.h"
#include "strutil.h"

#include <cstring>
#include <limits>

namespace {

constexpr size_t kAllocationPrefix = sizeof(GUEST_PTR);

const char *resolveNameByOrdinal(uint16_t ordinal) {
	switch (ordinal) {
	case 2:
		return "SysAllocString";
	case 4:
		return "SysAllocStringLen";
	case 6:
		return "SysFreeString";
	case 7:
		return "SysStringLen";
	default:
		return nullptr;
	}
}

} // namespace

namespace oleaut32 {

LPWSTR WINAPI SysAllocStringLen(LPCWSTR strIn, UINT ui) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysAllocStringLen(%p, %u)\n", strIn, ui);
	const uint64_t byteLength = static_cast<uint64_t>(ui) * sizeof(uint16_t);
	const uint64_t allocationSize = kAllocationPrefix + byteLength + sizeof(uint16_t);
	if (byteLength > std::numeric_limits<DWORD>::max() || allocationSize > std::numeric_limits<SIZE_T>::max()) {
		return nullptr;
	}
	auto *allocation = static_cast<uint8_t *>(wibo::heap::guestMalloc(static_cast<size_t>(allocationSize)));
	if (!allocation) {
		return nullptr;
	}
	auto *text = reinterpret_cast<LPWSTR>(allocation + kAllocationPrefix);
	// The byte length immediately precedes the text; the allocation prefix preserves pointer alignment.
	reinterpret_cast<DWORD *>(text)[-1] = static_cast<DWORD>(byteLength);
	if (strIn && ui) {
		std::memcpy(text, strIn, static_cast<size_t>(byteLength));
	}
	text[ui] = 0;
	return text;
}

LPWSTR WINAPI SysAllocString(LPCWSTR psz) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysAllocString(%p)\n", psz);
	if (!psz) {
		return nullptr;
	}
	const size_t length = wstrlen(psz);
	return length <= std::numeric_limits<UINT>::max() ? SysAllocStringLen(psz, static_cast<UINT>(length)) : nullptr;
}

void WINAPI SysFreeString(LPWSTR bstrString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysFreeString(%p)\n", bstrString);
	if (bstrString) {
		wibo::heap::guestFree(reinterpret_cast<uint8_t *>(bstrString) - kAllocationPrefix);
	}
}

UINT WINAPI SysStringLen(LPWSTR pbstr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysStringLen(%p)\n", pbstr);
	return pbstr ? reinterpret_cast<const DWORD *>(pbstr)[-1] / sizeof(uint16_t) : 0;
}

} // namespace oleaut32

#include "oleaut32_trampolines.h"

extern const wibo::ModuleStub lib_oleaut32 = {
	(const char *[]){"oleaut32", nullptr},
	oleaut32ThunkByName,
	resolveNameByOrdinal,
};
