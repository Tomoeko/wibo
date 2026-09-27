#include "ntdll.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/fileapi.h"
#include "kernel32/internal.h"

#include <cstring>
#include <limits>
#include <unistd.h>

namespace ntdll {
NTSTATUS WINAPI NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK ioStatus, PVOID information, ULONG length,
									 FILE_INFORMATION_CLASS informationClass) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtSetInformationFile(%p, %p, %p, %u, %u)\n", file, ioStatus, information, length,
			  static_cast<unsigned>(informationClass));
	if (!ioStatus)
		return STATUS_ACCESS_VIOLATION;
	if (static_cast<LONG_PTR>(file) < 0) {
		ioStatus->Status = STATUS_OBJECT_TYPE_MISMATCH;
		return ioStatus->Status;
	}
	HandleMeta meta{};
	auto object = wibo::handles().getAs<kernel32::FileObject>(file, &meta);
	if (!object || !object->valid()) {
		ioStatus->Status = STATUS_INVALID_HANDLE;
		return ioStatus->Status;
	}
	ioStatus->Information = 0;
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	if (informationClass == FilePositionInformation || informationClass == FileEndOfFileInformation) {
		if (length < sizeof(LARGE_INTEGER)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
		} else if (!information) {
			status = STATUS_ACCESS_VIOLATION;
		} else if (object->isPipe) {
			status = STATUS_NOT_SUPPORTED;
		} else {
			LARGE_INTEGER value;
			memcpy(&value, information, sizeof(value));
			if (value.QuadPart < 0 || value.QuadPart > std::numeric_limits<off_t>::max()) {
				status = STATUS_INVALID_PARAMETER;
			} else if (informationClass == FilePositionInformation) {
				if (!(meta.grantedAccess & (FILE_READ_DATA | FILE_WRITE_DATA))) {
					status = STATUS_ACCESS_DENIED;
				} else if (object->overlapped) {
					status = STATUS_INVALID_PARAMETER;
				} else {
					std::lock_guard lock(object->m);
					off_t position = 0;
					status = wibo::statusFromWinError(
						files::seekPositionLocked(*object, value.QuadPart, FILE_BEGIN, position));
				}
			} else if (!(meta.grantedAccess & FILE_WRITE_DATA)) {
				status = STATUS_ACCESS_DENIED;
			} else {
				std::lock_guard lock(object->m);
				status = ftruncate(object->fd, static_cast<off_t>(value.QuadPart)) == 0 ? STATUS_SUCCESS
																						: wibo::statusFromErrno(errno);
			}
		}
	}
	ioStatus->Status = status;
	return status;
}
} // namespace ntdll
