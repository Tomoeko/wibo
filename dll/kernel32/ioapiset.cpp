#include "ioapiset.h"

#include "context.h"
#include "directory_changes.h"
#include "errors.h"
#include "internal.h"
#include "namedpipeapi.h"
#include "ntdll.h"
#include "overlapped_util.h"
#include "synchapi.h"
#include "ws2/async_io.h"

#include <mutex>

namespace kernel32 {
BOOL WINAPI DeviceIoControl(HANDLE device, DWORD controlCode, LPVOID input, DWORD inputSize, LPVOID output,
							DWORD outputSize, LPDWORD bytesReturned, LPOVERLAPPED overlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DeviceIoControl(%p, 0x%x, %p, %u, %p, %u, %p, %p)\n", device, controlCode, input, inputSize, output,
			  outputSize, bytesReturned, overlapped);
	if (bytesReturned)
		*bytesReturned = 0;
	if ((!overlapped && !bytesReturned) || (inputSize && !input) || (outputSize && !output)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto file = wibo::handles().getAs<FileObject>(device);
	if (!file || !file->valid()) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	const bool asynchronousHandle = file->overlapped && overlapped;
	if (file->overlapped && !overlapped) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (asynchronousHandle) {
		overlapped->Internal = STATUS_PENDING;
		overlapped->InternalHigh = 0;
		detail::resetOverlappedEvent(overlapped);
	}
	IO_STATUS_BLOCK statusBlock{};
	const NTSTATUS status = ntdll::NtDeviceIoControlFile(device, NO_HANDLE, nullptr, nullptr, &statusBlock, controlCode,
														 input, inputSize, output, outputSize);
	const size_t transferred = status == STATUS_SUCCESS ? statusBlock.Information : 0;
	if (bytesReturned)
		*bytesReturned = static_cast<DWORD>(transferred);
	if (asynchronousHandle)
		detail::signalOverlappedEvent(file.get(), overlapped, status, transferred);
	if (status == STATUS_SUCCESS)
		return TRUE;
	if (status == STATUS_INFO_LENGTH_MISMATCH)
		setLastError(ERROR_INSUFFICIENT_BUFFER);
	else if (status == STATUS_ACCESS_DENIED)
		setLastError(ERROR_ACCESS_DENIED);
	else
		setLastError(wibo::winErrorFromNtStatus(status));
	return FALSE;
}

BOOL WINAPI CancelIo(HANDLE handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CancelIo(%p)\n", handle);
	if (auto directory = wibo::handles().getAs<DirectoryObject>(handle); directory && directory->valid()) {
		cancelDirectoryChangesForThread(*directory, pthread_self());
		return TRUE;
	}
	if (cancelNamedPipeConnect(handle, nullptr, true) != NamedPipeCancelResult::NotPipe)
		return TRUE;
	if (auto file = wibo::handles().getAs<FileObject>(handle); file && file->valid()) {
		if (file->overlapped) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
		return TRUE;
	}
	const auto socket = ws2::detail::findSocket(static_cast<SOCKET>(handle));
	if (socket) {
		ws2::detail::cancelBlockingSocketIoForThread(*socket);
		ws2::detail::cancelSocketIoForThread(socket, pthread_self());
		return TRUE;
	}
	setLastError(ERROR_INVALID_HANDLE);
	return FALSE;
}

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
	const auto pipeCancellation = cancelNamedPipeConnect(handle, overlapped, false);
	if (pipeCancellation != NamedPipeCancelResult::NotPipe) {
		if (pipeCancellation == NamedPipeCancelResult::Cancelled)
			return TRUE;
		setLastError(1168); // ERROR_NOT_FOUND
		return FALSE;
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
