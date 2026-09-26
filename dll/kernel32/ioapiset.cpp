#include "ioapiset.h"

#include "context.h"
#include "directory_changes.h"
#include "errors.h"
#include "internal.h"
#include "overlapped_util.h"
#include "synchapi.h"
#include "ws2/async_io.h"

#include <mutex>

namespace kernel32 {
BOOL WINAPI CancelIoEx(HANDLE handle, LPOVERLAPPED overlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CancelIoEx(%p, %p)\n", handle, overlapped);
	if (auto directory = wibo::handles().getAs<DirectoryObject>(handle); directory && directory->valid()) {
		if (!cancelDirectoryChanges(*directory, overlapped)) {
			setLastError(1168); // ERROR_NOT_FOUND
			return FALSE;
		}
		return TRUE;
	}
	if (auto file = wibo::handles().getAs<FileObject>(handle); file && file->valid()) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const auto socket = ws2::detail::findSocket(static_cast<SOCKET>(handle));
	if (!socket) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!ws2::detail::cancelSocketIo(socket, overlapped)) {
		setLastError(1168); // ERROR_NOT_FOUND
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
								  BOOL bWait) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetOverlappedResult(%p, %p, %p, %d)\n", hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
	if (!lpOverlapped) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (bWait && detail::loadOverlappedStatus(lpOverlapped) == STATUS_PENDING) {
		if (HANDLE waitHandle = kernel32::detail::normalizedOverlappedEventHandle(lpOverlapped)) {
			WaitForSingleObject(waitHandle, INFINITE);
		} else if (auto file = wibo::handles().getAs<FsObject>(hFile)) {
			std::unique_lock lk(file->overlappedMutex);
			CompletionWait completionWait;
			file->overlappedCv.wait(lk, [&] { return detail::loadOverlappedStatus(lpOverlapped) != STATUS_PENDING; });
		} else {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
	}

	const auto status = detail::loadOverlappedStatus(lpOverlapped);
	if (status == STATUS_PENDING) {
		setLastError(ERROR_IO_INCOMPLETE);
		return FALSE;
	}

	if (lpNumberOfBytesTransferred) {
		*lpNumberOfBytesTransferred = static_cast<DWORD>(detail::loadOverlappedBytes(lpOverlapped));
	}

	DWORD error = wibo::winErrorFromNtStatus(status);
	if (error == ERROR_SUCCESS) {
		return TRUE;
	}
	setLastError(error);
	return FALSE;
}

} // namespace kernel32
