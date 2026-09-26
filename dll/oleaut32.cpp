#include "oleaut32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "modules.h"
#include "strutil.h"

#include <cstring>
#include <limits>
#include <string>

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
	case 12: return "VariantChangeType";
	case 8: return "VariantInit";
	case 9: return "VariantClear";
	case 7:
		return "SysStringLen";
	default:
		return nullptr;
	}
}

} // namespace

namespace oleaut32 {

void WINAPI VariantInit(AutomationVariant *value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VariantInit(%p)\n", value);
	if (value) value->type = 0;
}

HRESULT WINAPI VariantClear(AutomationVariant *value) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VariantClear(%p)\n", value);
	if (!value) return static_cast<HRESULT>(0x80070057);
	const WORD type = value->type;
	const WORD base = type & 0xFFF;
	const bool scalar = base <= 8 || base == 10 || base == 11 || base == 14 || (base >= 16 && base <= 23);
	const bool indirect = (type & 0x4000) && (scalar || base == 9 || base == 12 || base == 13 || base == 36);
	if (type & 0x9000) return static_cast<HRESULT>(0x80020008);
	if (type & 0x2000) return static_cast<HRESULT>(0x80004001); // SAFEARRAY ownership is not implemented.
	if (!scalar && !indirect) {
		return static_cast<HRESULT>(base == 9 || base == 13 || base == 36 ? 0x80004001 : 0x80020008);
	}
	if (!(type & 0x4000) && base == 8) SysFreeString(fromGuestPtr<WCHAR>(value->value.pointer));
	value->type = 0;
	return S_OK;
}


HRESULT WINAPI VariantChangeType(AutomationVariant *destination, const AutomationVariant *source, WORD flags, WORD type) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VariantChangeType(%p, %p, 0x%x, %u), source type %u\n", destination, source, flags, type, source ? source->type : 0);
	if (!destination || !source) return static_cast<HRESULT>(0x80070057);
	if (flags & ~WORD(3)) return static_cast<HRESULT>(0x80004001);
	AutomationVariant converted = *source;
	if (source->type & 0x4000) {
		converted.type &= ~WORD(0x4000);
		size_t size = 0;
		switch (converted.type) {
		case 8: size = sizeof(GUEST_PTR); break;
		case 16: case 17: size = 1; break;
		case 2: case 11: case 18: size = 2; break;
		case 3: case 4: case 10: case 19: case 22: case 23: size = 4; break;
		case 5: case 6: case 7: case 20: case 21: size = 8; break;
		default: return static_cast<HRESULT>(0x80020005);
		}
		if (!source->value.pointer) return static_cast<HRESULT>(0x80070057);
		converted.value.scalar = 0;
		std::memcpy(&converted.value.scalar, fromGuestPtr(source->value.pointer), size);
	}
	if (type == 8) {
		LPWSTR text = nullptr;
		if (converted.type == 8) {
			const auto *original = fromGuestPtr<WCHAR>(converted.value.pointer);
			text = SysAllocStringLen(original, SysStringLen(const_cast<LPWSTR>(original)));
		} else {
			std::string digits;
			switch (converted.type) {
			case 0: break;
			case 2: digits = std::to_string(static_cast<int16_t>(converted.value.scalar)); break;
			case 3: case 22: digits = std::to_string(static_cast<int32_t>(converted.value.scalar)); break;
			case 11:
				digits = flags & 2 ? (converted.value.scalar & 0xFFFF ? "True" : "False") :
					std::to_string(static_cast<int16_t>(converted.value.scalar));
				break;
			case 16: digits = std::to_string(static_cast<int8_t>(converted.value.scalar)); break;
			case 17: digits = std::to_string(static_cast<uint8_t>(converted.value.scalar)); break;
			case 18: digits = std::to_string(static_cast<uint16_t>(converted.value.scalar)); break;
			case 19: case 23: digits = std::to_string(static_cast<uint32_t>(converted.value.scalar)); break;
			case 20: digits = std::to_string(static_cast<int64_t>(converted.value.scalar)); break;
			case 21: digits = std::to_string(converted.value.scalar); break;
			default: return static_cast<HRESULT>(0x80020005);
			}
			const std::u16string wide(digits.begin(), digits.end());
			text = SysAllocStringLen(reinterpret_cast<LPCWSTR>(wide.data()), static_cast<UINT>(wide.size()));
		}
		if (!text) return static_cast<HRESULT>(0x8007000E);
		converted = {};
		converted.type = 8;
		converted.value.pointer = toGuestPtr(text);
	} else {
		const bool plain = type <= 7 || type == 10 || type == 11 || type == 14 || (type >= 16 && type <= 23);
		if (!plain) return static_cast<HRESULT>(0x80020008);
		if (converted.type != type) return static_cast<HRESULT>(0x80020005);
	}
	const HRESULT cleared = VariantClear(destination);
	if (cleared != S_OK) {
		VariantClear(&converted);
		return cleared;
	}
	*destination = converted;
	return S_OK;
}

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
