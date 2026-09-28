#include "errors.h"

#include "common.h"

#include <cerrno>

namespace wibo {

DWORD winErrorFromErrno(int err) {
	switch (err) {
	case 0:
		return ERROR_SUCCESS;
	case EACCES:
	case EPERM:
		return ERROR_ACCESS_DENIED;
	case EXDEV:
		return ERROR_NOT_SAME_DEVICE;
	case ENOTEMPTY:
		return ERROR_DIR_NOT_EMPTY;
	case EEXIST:
		return ERROR_ALREADY_EXISTS;
	case ENOENT:
		return ERROR_FILE_NOT_FOUND;
	case ENOTDIR:
		return ERROR_PATH_NOT_FOUND;
	case ENOMEM:
		return ERROR_NOT_ENOUGH_MEMORY;
	case ENOSPC:
		return ERROR_DISK_FULL;
	case EINVAL:
		return ERROR_INVALID_PARAMETER;
	case EINTR:
		return ERROR_OPERATION_ABORTED;
	case EIO:
		return ERROR_READ_FAULT;
	case EPIPE:
		return ERROR_BROKEN_PIPE;
	case ESPIPE:
		return ERROR_INVALID_PARAMETER;
	default:
		DEBUG_LOG("Unhandled errno %d -> ERROR_NOT_SUPPORTED\n", err);
		return ERROR_NOT_SUPPORTED;
	}
}

NTSTATUS statusFromWinError(DWORD error) {
	switch (error) {
	case ERROR_SUCCESS:
		return STATUS_SUCCESS;
	case ERROR_OPERATION_ABORTED:
		return STATUS_CANCELLED;
	case ERROR_INVALID_HANDLE:
		return STATUS_INVALID_HANDLE;
	case ERROR_INVALID_PARAMETER:
		return STATUS_INVALID_PARAMETER;
	case ERROR_ACCESS_DENIED:
		return STATUS_ACCESS_DENIED;
	case ERROR_NOT_SUPPORTED:
		return STATUS_NOT_SUPPORTED;
	case ERROR_BROKEN_PIPE:
		return STATUS_PIPE_BROKEN;
	case ERROR_LOCK_VIOLATION:
		return STATUS_FILE_LOCK_CONFLICT;
	case ERROR_HANDLE_EOF:
		return STATUS_END_OF_FILE;
	default:
		return STATUS_UNEXPECTED_IO_ERROR;
	}
}

NTSTATUS statusFromErrno(int err) { return statusFromWinError(winErrorFromErrno(err)); }

DWORD winErrorFromNtStatus(NTSTATUS status) {
	switch (status) {
	case STATUS_HANDLES_CLOSED:
		return ERROR_HANDLES_CLOSED;
	case STATUS_SUCCESS:
		return ERROR_SUCCESS;
	case STATUS_FILE_LOCK_CONFLICT:
		return ERROR_LOCK_VIOLATION;
	case STATUS_PENDING:
		return ERROR_IO_PENDING;
	case STATUS_END_OF_FILE:
		return ERROR_HANDLE_EOF;
	case STATUS_CANCELLED:
		return ERROR_OPERATION_ABORTED;
	case STATUS_INVALID_HANDLE:
		return ERROR_INVALID_HANDLE;
	case STATUS_INVALID_PARAMETER:
		return ERROR_INVALID_PARAMETER;
	case STATUS_CONNECTION_RESET:
		return ERROR_NETNAME_DELETED;
	case STATUS_BUFFER_OVERFLOW:
		return ERROR_MORE_DATA;
	case STATUS_PIPE_BROKEN:
		return ERROR_BROKEN_PIPE;
	default:
		return ERROR_NOT_SUPPORTED;
	}
}

} // namespace wibo
