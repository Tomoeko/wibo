#include "eventlog.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "strutil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr size_t kMaxSourceLength = 1024;
constexpr size_t kMaxDiagnosticLength = 16384;
constexpr size_t kMaxRawDataLength = 4096;
constexpr WORD kMaxStrings = 16;

struct EventLogSource final : ObjectBase {
	static constexpr ObjectType kType = ObjectType::EventLog;
	std::string source;

	explicit EventLogSource(std::string name) : ObjectBase(kType), source(std::move(name)) {}
};

bool diagnosticSinkEnabled() {
	const char *value = std::getenv("WIBO_EVENT_LOG_STDERR");
	return value && std::strcmp(value, "1") == 0;
}

size_t boundedWideLength(LPCWSTR value, size_t maximum) {
	for (size_t length = 0; length <= maximum; ++length) {
		if (!value[length])
			return length;
	}
	return maximum + 1;
}

void appendEscaped(std::string &output, std::string_view value) {
	constexpr char digits[] = "0123456789abcdef";
	for (unsigned char byte : value) {
		switch (byte) {
		case '\\':
		case '"':
			output.push_back('\\');
			output.push_back(static_cast<char>(byte));
			break;
		case '\n':
			output += "\\n";
			break;
		case '\r':
			output += "\\r";
			break;
		default:
			if (byte < 0x20 || byte == 0x7f) {
				output += "\\x";
				output.push_back(digits[byte >> 4]);
				output.push_back(digits[byte & 0xf]);
			} else {
				output.push_back(static_cast<char>(byte));
			}
		}
	}
}

} // namespace

namespace advapi32 {

HANDLE WINAPI RegisterEventSourceW(LPCWSTR serverName, LPCWSTR sourceName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterEventSourceW(%p, %p)\n", serverName, sourceName);
	if (serverName && *serverName) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	if (!sourceName) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	const size_t length = boundedWideLength(sourceName, kMaxSourceLength);
	if (length > kMaxSourceLength) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	std::string source;
	if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(sourceName), length), source)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	auto eventLog = make_pin<EventLogSource>(std::move(source));
	if (!eventLog) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NO_HANDLE;
	}
	return wibo::handles().alloc(std::move(eventLog), 0, 0);
}

BOOL WINAPI ReportEventW(HANDLE eventLog, WORD type, WORD category, DWORD eventId, PSID userSid, WORD stringCount,
						 DWORD dataSize, const GUEST_PTR *strings, LPVOID rawData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReportEventW(%p, %u, %u, %u, %p, %u, %u, %p, %p)\n", eventLog, type, category, eventId, userSid,
			  stringCount, dataSize, strings, rawData);
	auto source = wibo::handles().getAs<EventLogSource>(eventLog);
	if (!source) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!diagnosticSinkEnabled()) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (userSid || stringCount > kMaxStrings || dataSize > kMaxRawDataLength) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if ((stringCount && !strings) || (dataSize && !rawData)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char metadata[128];
	const int count = std::snprintf(metadata, sizeof(metadata), " type=%u category=%u id=%u strings=%u data=", type,
									category, eventId, stringCount);
	if (count < 0 || static_cast<size_t>(count) >= sizeof(metadata)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::string record = "EventLog source=\"";
	appendEscaped(record, source->source);
	record += '"';
	record.append(metadata, static_cast<size_t>(count));
	record += std::to_string(dataSize);
	for (WORD index = 0; index < stringCount; ++index) {
		const LPCWSTR value = fromGuestPtr<const WCHAR>(strings[index]);
		if (!value) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		const size_t length = boundedWideLength(value, kMaxDiagnosticLength);
		if (length > kMaxDiagnosticLength) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
		std::string converted;
		if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(value), length), converted)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		record += " text=\"";
		appendEscaped(record, converted);
		record += '"';
		if (record.size() > kMaxDiagnosticLength) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	if (dataSize) {
		constexpr char digits[] = "0123456789abcdef";
		const auto *bytes = static_cast<const unsigned char *>(rawData);
		record += " raw=";
		for (DWORD index = 0; index < dataSize; ++index) {
			record.push_back(digits[bytes[index] >> 4]);
			record.push_back(digits[bytes[index] & 0xf]);
		}
	}
	if (record.size() > kMaxDiagnosticLength) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	record += '\n';
	if (std::fwrite(record.data(), 1, record.size(), stderr) != record.size() || std::fflush(stderr) != 0) {
		kernel32::setLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DeregisterEventSource(HANDLE eventLog) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DeregisterEventSource(%p)\n", eventLog);
	if (!wibo::handles().getAs<EventLogSource>(eventLog) || !wibo::handles().release(eventLog)) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

} // namespace advapi32
