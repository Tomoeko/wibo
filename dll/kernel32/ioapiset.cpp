#include "ioapiset.h"

#include "context.h"
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

	if (bWait && lpOverlapped->Internal == STATUS_PENDING) {
		if (HANDLE waitHandle = kernel32::detail::normalizedOverlappedEventHandle(lpOverlapped)) {
			WaitForSingleObject(waitHandle, INFINITE);
		} else if (auto file = wibo::handles().getAs<FileObject>(hFile)) {
			std::unique_lock lk(file->m);
			CompletionWait completionWait;
			file->overlappedCv.wait(lk, [&] { return lpOverlapped->Internal != STATUS_PENDING; });
		} else {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
	}

	const auto status = static_cast<NTSTATUS>(lpOverlapped->Internal);
	if (status == STATUS_PENDING) {
		setLastError(ERROR_IO_INCOMPLETE);
		return FALSE;
	}

	if (lpNumberOfBytesTransferred) {
		*lpNumberOfBytesTransferred = static_cast<DWORD>(lpOverlapped->InternalHigh);
	}

	DWORD error = wibo::winErrorFromNtStatus(status);
	if (error == ERROR_SUCCESS) {
		return TRUE;
	}
	setLastError(error);
	return FALSE;
}

} // namespace kernel32
