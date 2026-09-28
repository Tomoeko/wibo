#include "volumeapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "ntdll.h"

#include <array>
#include <cstring>

namespace {

constexpr size_t kVolumeLabelOffset = 18;
constexpr size_t kFileSystemNameOffset = 12;

bool queryVolume(HANDLE file, ULONG informationClass, std::array<BYTE, 4096> &buffer, size_t &used) {
	IO_STATUS_BLOCK result{};
	const NTSTATUS status =
		ntdll::NtQueryVolumeInformationFile(file, &result, buffer.data(), buffer.size(), informationClass);
	if (status != STATUS_SUCCESS) {
		DWORD error = wibo::winErrorFromNtStatus(status);
		if (status == STATUS_ACCESS_DENIED)
			error = ERROR_ACCESS_DENIED;
		else if (status == STATUS_INFO_LENGTH_MISMATCH)
			error = ERROR_INSUFFICIENT_BUFFER;
		kernel32::setLastError(error);
		return false;
	}
	used = static_cast<size_t>(result.Information);
	if (used > buffer.size()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return false;
	}
	return true;
}

DWORD readDword(const BYTE *bytes) {
	DWORD value;
	std::memcpy(&value, bytes, sizeof(value));
	return value;
}

bool copyName(LPWSTR output, DWORD capacity, const BYTE *source, DWORD bytes) {
	if (!output)
		return true;
	if (bytes % sizeof(WCHAR) != 0 || bytes / sizeof(WCHAR) >= capacity) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return false;
	}
	std::memcpy(output, source, bytes);
	output[bytes / sizeof(WCHAR)] = 0;
	return true;
}

} // namespace

namespace kernel32 {

BOOL WINAPI GetVolumeInformationByHandleW(HANDLE file, LPWSTR volumeName, DWORD volumeNameSize, LPDWORD serialNumber,
										  LPDWORD maximumComponentLength, LPDWORD fileSystemFlags,
										  LPWSTR fileSystemName, DWORD fileSystemNameSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVolumeInformationByHandleW(%p, %p, %u, %p, %p, %p, %p, %u)\n", file, volumeName, volumeNameSize,
			  serialNumber, maximumComponentLength, fileSystemFlags, fileSystemName, fileSystemNameSize);
	std::array<BYTE, 4096> volume{};
	std::array<BYTE, 4096> attributes{};
	size_t volumeUsed = 0, attributesUsed = 0;
	if (!queryVolume(file, 1, volume, volumeUsed) || !queryVolume(file, 5, attributes, attributesUsed))
		return FALSE;
	if (volumeUsed < kVolumeLabelOffset || attributesUsed < kFileSystemNameOffset) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	const DWORD labelBytes = readDword(volume.data() + 12);
	const DWORD nameBytes = readDword(attributes.data() + 8);
	if ((labelBytes & 1) || (nameBytes & 1) || labelBytes > volumeUsed - kVolumeLabelOffset ||
		nameBytes > attributesUsed - kFileSystemNameOffset) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (!copyName(volumeName, volumeNameSize, volume.data() + kVolumeLabelOffset, labelBytes) ||
		!copyName(fileSystemName, fileSystemNameSize, attributes.data() + kFileSystemNameOffset, nameBytes))
		return FALSE;
	if (serialNumber)
		*serialNumber = readDword(volume.data() + 8);
	if (maximumComponentLength)
		*maximumComponentLength = readDword(attributes.data() + 4);
	if (fileSystemFlags)
		*fileSystemFlags = readDword(attributes.data());
	return TRUE;
}

} // namespace kernel32
