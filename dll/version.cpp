#include "version.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"
#include "system_provider.h"
#include "types.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr uint32_t RT_VERSION = 16;
constexpr size_t kMaxVersionRequest = 64 * 1024;
constexpr size_t kMaxVersionPathUnits = (kMaxVersionRequest - 128) / 4;
// Aligned VS_VERSION_INFO header/key followed by VS_FIXEDFILEINFO.
constexpr DWORD kMinVersionBuffer = 92;

bool encodeVersionFileName(LPCWSTR filename, std::string &encoded) {
	encoded = "-";
	if (!filename)
		return true;
	const size_t units = wstrnlen(filename, kMaxVersionPathUnits);
	if (units == kMaxVersionPathUnits) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	if (!units) {
		encoded.clear();
		return true;
	}
	std::u16string name;
	name.reserve(units);
	for (size_t index = 0; index < units; ++index)
		name.push_back(static_cast<char16_t>(filename[index]));
	std::string path;
	if (!utf16ToUtf8(name, path)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	std::string normalized = path;
	std::replace(normalized.begin(), normalized.end(), '\\', '/');
	if (normalized.starts_with("//?/"))
		normalized.erase(0, 4);
	const bool rootedDrive =
		normalized.size() >= 3 && normalized[1] == ':' && normalized[2] == '/' &&
		(normalized[0] == 'C' || normalized[0] == 'c' || normalized[0] == 'Z' || normalized[0] == 'z');
	const bool rootedHost = !path.empty() && path[0] == '/' && !normalized.empty() && normalized[0] == '/' &&
							(normalized.size() == 1 || normalized[1] != '/');
	if (!rootedDrive && !rootedHost) {
		// Relative paths require the guest loader's search context.
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	const char *configured = std::getenv("WIBO_SYSTEM_PROVIDER_HOST_ROOT");
	if (!configured || !*configured) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	const size_t rootLength = strnlen(configured, kMaxVersionPathUnits);
	if (rootLength == kMaxVersionPathUnits) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	std::string root(configured, rootLength);
	std::replace(root.begin(), root.end(), '/', '\\');
	const bool driveLetter = (root[0] >= 'A' && root[0] <= 'Z') || (root[0] >= 'a' && root[0] <= 'z');
	if (root.size() < 2 || !driveLetter || root[1] != ':' || (root.size() > 2 && root[2] != '\\') ||
		root.find(':', 2) != std::string::npos) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	while (root.size() > 2 && root.back() == '\\')
		root.pop_back();
	std::error_code error;
	const auto absolute = std::filesystem::absolute(files::pathFromWindows(path.c_str()), error);
	if (error) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	const auto canonical = std::filesystem::weakly_canonical(absolute, error);
	const std::string physical = (error ? absolute.lexically_normal() : canonical).generic_string();
	if (physical.find_first_of(":\\") != std::string::npos) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	// This setting declares the provider namespace corresponding to the host root.
	auto backend = utf8ToUtf16(root + physical);
	if (!backend) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	if (backend->size() > kMaxVersionPathUnits) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	std::string bytes;
	bytes.reserve(backend->size() * sizeof(WCHAR));
	for (char16_t value : *backend) {
		if (value == '/')
			value = '\\';
		bytes.push_back(static_cast<char>(value & 0xff));
		bytes.push_back(static_cast<char>(value >> 8));
	}
	encoded = wibo::provider::encodeBytes(bytes);
	return true;
}

uint16_t readU16(const uint8_t *ptr) { return static_cast<uint16_t>(ptr[0] | (ptr[1] << 8)); }

size_t align4(size_t offset) { return (offset + 3u) & ~static_cast<size_t>(3u); }

std::string narrowKey(const std::u16string &key) {
	std::string result;
	result.reserve(key.size());
	for (char16_t ch : key) {
		result.push_back(static_cast<char>(ch & 0xFF));
	}
	return result;
}

struct VersionBlockView {
	uint16_t totalLength = 0;
	uint16_t valueLength = 0;
	uint16_t type = 0;
	std::u16string key;
	const uint8_t *valuePtr = nullptr;
	uint32_t valueBytes = 0;
	const uint8_t *childrenPtr = nullptr;
	uint32_t childrenBytes = 0;
};

bool parseVersionBlock(const uint8_t *block, size_t available, VersionBlockView &out) {
	if (available < sizeof(uint16_t) * 3) {
		DEBUG_LOG("header too small: available=%zu\n", available);
		return false;
	}

	uint16_t totalLength = readU16(block);
	uint16_t valueLength = readU16(block + sizeof(uint16_t));
	uint16_t type = readU16(block + sizeof(uint16_t) * 2);
	if (totalLength == 0 || totalLength > available) {
		DEBUG_LOG("invalid totalLength=%u available=%zu\n", totalLength, available);
		return false;
	}

	const uint8_t *end = block + totalLength;
	const uint8_t *cursor = block + sizeof(uint16_t) * 3;
	out.key.clear();
	while (cursor + sizeof(uint16_t) <= end) {
		uint16_t ch = readU16(cursor);
		cursor += sizeof(uint16_t);
		if (!ch)
			break;
		out.key.push_back(static_cast<char16_t>(ch));
	}
	DEBUG_LOG("parsed key fragment=%s\n", narrowKey(out.key).c_str());

	cursor = block + sizeof(uint16_t) * 3 + (out.key.size() + 1) * sizeof(uint16_t);
	if (cursor > end) {
		DEBUG_LOG("key cursor beyond block: cursor=%zu end=%zu\n", static_cast<size_t>(cursor - block),
				  static_cast<size_t>(end - block));
		return false;
	}

	cursor = block + align4(static_cast<size_t>(cursor - block));

	uint32_t valueBytes = 0;
	if (valueLength) {
		valueBytes =
			type == 1 ? static_cast<uint32_t>(valueLength) * sizeof(uint16_t) : static_cast<uint32_t>(valueLength);
		if (cursor + valueBytes > end) {
			DEBUG_LOG("value beyond block: bytes=%u remaining=%zu\n", valueBytes, static_cast<size_t>(end - cursor));
			return false;
		}
	}

	const uint8_t *children = block + align4(static_cast<size_t>((cursor + valueBytes) - block));
	if (children > end)
		children = end;

	out.totalLength = totalLength;
	out.valueLength = valueLength;
	out.type = type;
	out.valuePtr = valueLength ? cursor : nullptr;
	out.valueBytes = valueBytes;
	out.childrenPtr = children;
	out.childrenBytes = static_cast<uint32_t>(end - children);
	return true;
}

bool queryVersionBlock(const uint8_t *block, size_t available, const std::vector<std::string> &segments, size_t depth,
					   const uint8_t **outPtr, uint32_t *outLen, uint16_t *outType) {
	VersionBlockView view;
	if (!parseVersionBlock(block, available, view))
		return false;

	if (depth == segments.size()) {
		if (outPtr)
			*outPtr = view.valueBytes ? view.valuePtr : nullptr;
		if (outLen)
			*outLen = view.type == 1 ? view.valueLength : view.valueBytes;
		if (outType)
			*outType = view.type;
		return true;
	}

	const std::string targetLower = stringToLower(segments[depth]);
	const uint8_t *cursor = view.childrenPtr;
	const uint8_t *end = view.childrenPtr + view.childrenBytes;

	while (cursor + 6 <= end) {
		const uint8_t *childStart = cursor;
		VersionBlockView child;
		if (!parseVersionBlock(cursor, static_cast<size_t>(end - cursor), child))
			break;
		if (child.totalLength == 0)
			break;
		std::string childKeyLower = stringToLower(narrowKey(child.key));
		if (childKeyLower == targetLower) {
			if (queryVersionBlock(childStart, child.totalLength, segments, depth + 1, outPtr, outLen, outType))
				return true;
		}
		const auto offset = static_cast<size_t>(child.totalLength);
		cursor = childStart + align4(offset);
		if (cursor <= childStart || cursor > end)
			break;
	}
	return false;
}

bool splitSubBlock(const std::string &subBlock, std::vector<std::string> &segments) {
	segments.clear();
	if (subBlock.empty() || subBlock == "\\")
		return true;

	const char *cursor = subBlock.c_str();
	if (*cursor == '\\')
		++cursor;

	while (*cursor) {
		const char *next = std::strchr(cursor, '\\');
		if (!next)
			next = cursor + std::strlen(cursor);
		segments.emplace_back(cursor, static_cast<size_t>(next - cursor));
		cursor = *next ? next + 1 : next;
	}
	return true;
}

bool loadVersionResource(const char *fileName, std::vector<uint8_t> &buffer) {
	if (!fileName) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	auto hostPath = files::pathFromWindows(fileName);
	std::string hostPathStr = hostPath.string();
	FILE *fp = std::fopen(hostPathStr.c_str(), "rb");
	if (!fp) {
		kernel32::setLastError(ERROR_FILE_NOT_FOUND);
		return false;
	}

	wibo::Executable executable;
	if (!executable.loadPE(fp, false)) {
		std::fclose(fp);
		kernel32::setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}

	std::fclose(fp);

	wibo::ResourceIdentifier type = wibo::ResourceIdentifier::fromID(RT_VERSION);
	wibo::ResourceIdentifier name = wibo::ResourceIdentifier::fromID(1);
	wibo::ResourceLocation loc;
	if (!executable.findResource(type, name, std::nullopt, loc)) {
		auto nameString = wibo::ResourceIdentifier::fromString(u"VS_VERSION_INFO");
		if (!executable.findResource(type, nameString, std::nullopt, loc))
			return false;
	}

	const uint8_t *start = static_cast<const uint8_t *>(loc.data);
	buffer.assign(start, start + loc.size);
	return true;
}

} // namespace

namespace version {

UINT WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoSizeA(%s, %p)\n", lptstrFilename, lpdwHandle);
	if (lpdwHandle)
		*lpdwHandle = 0;

	std::vector<uint8_t> buffer;
	if (!loadVersionResource(lptstrFilename, buffer))
		return 0;
	return static_cast<unsigned int>(buffer.size());
}

UINT WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
	HOST_CONTEXT_GUARD();
	(void)dwHandle;
	DEBUG_LOG("GetFileVersionInfoA(%s, %u, %p)\n", lptstrFilename, dwLen, lpData);
	if (!lpData || dwLen == 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	std::vector<uint8_t> buffer;
	if (!loadVersionResource(lptstrFilename, buffer))
		return 0;

	if (buffer.size() > dwLen) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	std::memcpy(lpData, buffer.data(), buffer.size());
	if (buffer.size() < dwLen) {
		std::memset(static_cast<uint8_t *>(lpData) + buffer.size(), 0, dwLen - buffer.size());
	}
	return 1;
}

static unsigned int VerQueryValueImpl(const void *pBlock, const std::string &subBlock, GUEST_PTR *lplpBuffer,
									  unsigned int *puLen) {
	if (!pBlock)
		return 0;

	const auto *base = static_cast<const uint8_t *>(pBlock);
	uint16_t totalLength = readU16(base);
	if (totalLength < 6)
		return 0;

	std::vector<std::string> segments;
	if (!splitSubBlock(subBlock, segments))
		return 0;

	const uint8_t *outPtr = nullptr;
	uint32_t outLen = 0;
	uint16_t outType = 0;
	if (!queryVersionBlock(base, totalLength, segments, 0, &outPtr, &outLen, &outType))
		return 0;

	if (outType == 1 && outPtr) {
		char *dest = reinterpret_cast<char *>(const_cast<uint8_t *>(outPtr));
		std::string narrow = wideStringToString(reinterpret_cast<const uint16_t *>(outPtr), static_cast<int>(outLen));
		std::memcpy(dest, narrow.c_str(), narrow.size() + 1);
		if (lplpBuffer)
			*lplpBuffer = toGuestPtr(dest);
		if (puLen)
			*puLen = static_cast<unsigned int>(narrow.size());
		return 1;
	}

	if (lplpBuffer)
		*lplpBuffer = toGuestPtr(outPtr);
	if (puLen)
		*puLen = outLen;
	return 1;
}

UINT WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, GUEST_PTR *lplpBuffer, PUINT puLen) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerQueryValueA(%p, %s, %p, %p)\n", pBlock, lpSubBlock ? lpSubBlock : "(null)", lplpBuffer, puLen);
	if (!lpSubBlock)
		return 0;
	return VerQueryValueImpl(pBlock, lpSubBlock, lplpBuffer, puLen);
}

UINT WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoSizeW -> ");
	auto narrow = wideStringToString(lptstrFilename);
	return GetFileVersionInfoSizeA(narrow.c_str(), lpdwHandle);
}

DWORD WINAPI GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = kernel32::getLastError();
	DEBUG_LOG("GetFileVersionInfoSizeExW(0x%x, %p, %p)\n", dwFlags, lpwstrFilename, lpdwHandle);
	std::string filename;
	if (!encodeVersionFileName(lpwstrFilename, filename))
		return 0;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"file-version-info-size-ex-w", std::to_string(dwFlags), filename,
								  lpdwHandle ? "1" : "0", std::to_string(incomingError)},
								 response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	if (status != ERROR_SUCCESS) {
		DWORD error = status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status);
		if (!reader.done())
			error = ERROR_INVALID_DATA;
		kernel32::setLastError(error);
		return 0;
	}
	uint32_t size = 0, nativeError = 0, handlePresent = 0, handle = 0;
	if (!reader.number(size) || !reader.number(nativeError) || !reader.number(handlePresent) || handlePresent > 1 ||
		!reader.number(handle) || handle || (handlePresent && !lpdwHandle) || !reader.done()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	if (handlePresent)
		*lpdwHandle = handle;
	// LastError is part of this native result, including successful calls.
	kernel32::setLastError(nativeError);
	return size;
}

BOOL WINAPI GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = kernel32::getLastError();
	DEBUG_LOG("GetFileVersionInfoExW(0x%x, %p, %u, %u, %p)\n", dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
	if (lpData && dwLen < kMinVersionBuffer) {
		// Tiny buffers cannot transport native version-header inspection safely.
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::string filename;
	if (!encodeVersionFileName(lpwstrFilename, filename))
		return FALSE;
	constexpr size_t kRequestOverhead = 256;
	if (filename.size() > kMaxVersionRequest - kRequestOverhead ||
		(lpData && dwLen > (kMaxVersionRequest - kRequestOverhead - filename.size()) / 2)) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	const std::string input =
		lpData ? wibo::provider::encodeBytes(std::string_view(static_cast<const char *>(lpData), dwLen)) : "-";
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"file-version-info-ex-w", std::to_string(dwFlags), filename, std::to_string(dwHandle),
								  std::to_string(dwLen), input, std::to_string(incomingError)},
								 response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (status != ERROR_SUCCESS) {
		DWORD error = status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status);
		if (!reader.done())
			error = ERROR_INVALID_DATA;
		kernel32::setLastError(error);
		return FALSE;
	}
	uint32_t result = 0, nativeError = 0;
	std::vector<uint8_t> output;
	if (!reader.number(result) || result > 1 || !reader.number(nativeError) || !reader.bytes(output) ||
		!reader.done() || output.size() != (lpData ? dwLen : 0) || (result && !lpData)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (lpData)
		std::memcpy(lpData, output.data(), output.size());
	// A completed native call may modify the buffer even when it returns FALSE.
	kernel32::setLastError(nativeError);
	return result ? TRUE : FALSE;
}

UINT WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoW -> ");
	auto narrow = wideStringToString(lptstrFilename);
	return GetFileVersionInfoA(narrow.c_str(), dwHandle, dwLen, lpData);
}

UINT WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, GUEST_PTR *lplpBuffer, PUINT puLen) {
	HOST_CONTEXT_GUARD();
	if (!lpSubBlock)
		return 0;
	auto narrow = wideStringToString(lpSubBlock);
	DEBUG_LOG("VerQueryValueW(%p, %s, %p, %p)\n", pBlock, narrow.c_str(), lplpBuffer, puLen);
	return VerQueryValueImpl(pBlock, narrow, lplpBuffer, puLen);
}

} // namespace version

#include "version_trampolines.h"

extern const wibo::ModuleStub lib_version = {
	(const char *[]){
		"version",
		nullptr,
	},
	versionThunkByName,
	nullptr,
};
