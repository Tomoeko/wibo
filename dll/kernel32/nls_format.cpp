#include "winnls.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "strutil.h"
#include "system_provider.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr size_t kRequestOverhead = 256;
constexpr size_t kMaxSnapshotUnits = (64 * 1024 - kRequestOverhead) / (2 * sizeof(WCHAR));
constexpr size_t kResponseOverhead = 6 * sizeof(uint32_t);
static_assert(sizeof(SYSTEMTIME) == 16);
static_assert(kMaxSnapshotUnits * sizeof(WCHAR) <= wibo::provider::kMaxResponse - kResponseOverhead);

int failFormat(DWORD error) {
	kernel32::setLastError(error);
	return 0;
}

bool validRange(const void *buffer, size_t bytes) {
	return bytes <= std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(buffer);
}

bool overlaps(const void *left, size_t leftBytes, const void *right, size_t rightBytes) {
	const auto leftAddress = reinterpret_cast<uintptr_t>(left);
	const auto rightAddress = reinterpret_cast<uintptr_t>(right);
	return leftBytes && rightBytes && leftAddress < rightAddress + rightBytes && rightAddress < leftAddress + leftBytes;
}

int formatSnapshot(const char *operation, LCID locale, DWORD flags, const SYSTEMTIME *value, LPCWSTR picture,
				   LPWSTR output, int capacity) {
	const DWORD incomingError = kernel32::getLastError();
	if (capacity < 0 || (capacity && !output))
		return failFormat(ERROR_INVALID_PARAMETER);
	const size_t outputUnits = static_cast<size_t>(capacity);
	if (outputUnits > kMaxSnapshotUnits)
		return failFormat(ERROR_NOT_ENOUGH_MEMORY);
	const size_t outputBytes = outputUnits * sizeof(WCHAR);
	if (!validRange(output, outputBytes))
		return failFormat(ERROR_INVALID_PARAMETER);
	std::string timeBytes = "-";
	if (value) {
		if (!validRange(value, sizeof(SYSTEMTIME)))
			return failFormat(ERROR_INVALID_PARAMETER);
		if (overlaps(value, sizeof(SYSTEMTIME), output, outputBytes))
			return failFormat(ERROR_NOT_SUPPORTED);
		timeBytes =
			wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(value), sizeof(SYSTEMTIME)));
	}
	std::string formatBytes = "-";
	if (picture) {
		const size_t remainingUnits = kMaxSnapshotUnits - outputUnits;
		if (!validRange(picture, (remainingUnits + 1) * sizeof(WCHAR)))
			return failFormat(ERROR_INVALID_PARAMETER);
		const size_t units = wstrnlen(picture, remainingUnits + 1);
		if (units > remainingUnits)
			return failFormat(ERROR_NOT_ENOUGH_MEMORY);
		if (overlaps(picture, (units + 1) * sizeof(WCHAR), output, outputBytes))
			return failFormat(ERROR_NOT_SUPPORTED);
		formatBytes = wibo::provider::encodeBytes(
			std::string_view(reinterpret_cast<const char *>(picture), units * sizeof(WCHAR)));
	}
	const std::string initial =
		outputUnits ? wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(output), outputBytes))
					: "-";
	std::vector<uint8_t> response;
	if (!wibo::provider::request({operation, std::to_string(locale), std::to_string(flags), timeBytes, formatBytes,
								  std::to_string(capacity), initial, std::to_string(incomingError)},
								 response))
		return failFormat(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return failFormat(ERROR_INVALID_DATA);
	if (status) {
		if (!reader.done())
			return failFormat(ERROR_INVALID_DATA);
		return failFormat(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status));
	}
	uint32_t result = 0, nativeError = 0;
	std::vector<uint8_t> snapshot;
	if (!reader.number(result) || result > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		(outputUnits && result > outputUnits) || !reader.number(nativeError) || !reader.bytes(snapshot) ||
		!reader.done() || snapshot.size() != outputBytes)
		return failFormat(ERROR_INVALID_DATA);
	if (result && outputBytes) {
		WCHAR terminator;
		std::memcpy(&terminator, snapshot.data() + (result - 1) * sizeof(WCHAR), sizeof(terminator));
		if (terminator)
			return failFormat(ERROR_INVALID_DATA);
	}
	if (outputBytes)
		std::memcpy(output, snapshot.data(), outputBytes);
	kernel32::setLastError(nativeError);
	return static_cast<int>(result);
}

} // namespace

namespace kernel32 {

int WINAPI GetDateFormatW(LCID locale, DWORD flags, const SYSTEMTIME *date, LPCWSTR picture, LPWSTR output,
						  int capacity) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDateFormatW(%u, %u, %p, %p, %p, %d)\n", locale, flags, date, picture, output, capacity);
	return formatSnapshot("date-format-w", locale, flags, date, picture, output, capacity);
}

int WINAPI GetTimeFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR picture, LPWSTR output,
						  int capacity) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTimeFormatW(%u, %u, %p, %p, %p, %d)\n", locale, flags, time, picture, output, capacity);
	return formatSnapshot("time-format-w", locale, flags, time, picture, output, capacity);
}

} // namespace kernel32
