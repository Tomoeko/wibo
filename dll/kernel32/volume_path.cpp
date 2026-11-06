#include "fileapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"
#include "strutil.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace kernel32 {

BOOL WINAPI GetVolumePathNameW(LPCWSTR fileName, LPWSTR volumePathName, DWORD bufferLength) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	DEBUG_LOG("GetVolumePathNameW(%p, %p, %u)\n", fileName, volumePathName, bufferLength);
	if (!fileName || !volumePathName || !bufferLength) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	constexpr size_t kMaximumPathUnits = 32768;
	const size_t length = wstrnlen(fileName, kMaximumPathUnits);
	if (!length) {
		setLastError(ERROR_SUCCESS);
		return FALSE;
	}
	if (length == kMaximumPathUnits) {
		setLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}
	size_t prefix = 0;
	if (length >= 4 && fileName[0] == '\\' && fileName[1] == '\\' && (fileName[2] == '?' || fileName[2] == '.') &&
		fileName[3] == '\\')
		prefix = 4;
	if (length < prefix + 2 || fileName[prefix + 1] != ':') {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	WCHAR drive = fileName[prefix];
	if (drive >= 'a' && drive <= 'z')
		drive -= 'a' - 'A';
	if (drive != 'C' && drive != 'Z') {
		setLastError(ERROR_INVALID_DRIVE);
		return FALSE;
	}
	const char *root = drive == 'C' ? std::getenv("WIBO_C_DRIVE") : nullptr;
	if (!root || !root[0])
		root = "/";
	struct stat status {};
	if (stat(root, &status) != 0) {
		setLastError(wibo::winErrorFromErrno(errno));
		return FALSE;
	}
	if (!S_ISDIR(status.st_mode)) {
		setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	std::array<WCHAR, 8> result{};
	if (prefix)
		std::memcpy(result.data(), fileName, prefix * sizeof(WCHAR));
	result[prefix] = drive;
	result[prefix + 1] = ':';
	result[prefix + 2] = '\\';
	size_t resultLength = prefix + 3;
	if (bufferLength < resultLength) {
		setLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}
	if (bufferLength == resultLength)
		result[--resultLength] = 0;
	std::memcpy(volumePathName, result.data(), (resultLength + 1) * sizeof(WCHAR));
	setLastError(incomingError);
	return TRUE;
}

} // namespace kernel32
