#include "fileapi.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "internal.h"

#include <cstring>
#include <mutex>

namespace kernel32 {

BOOL WINAPI SetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS informationClass, LPVOID information,
									   DWORD length) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	DEBUG_LOG("SetFileInformationByHandle(%p, %u, %p, %u)\n", file, static_cast<unsigned>(informationClass),
			  information, length);
	if (informationClass < FileBasicInfo || informationClass >= MaximumFileInfoByHandleClass) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (informationClass != FileDispositionInfo) {
		setLastError(ERROR_CALL_NOT_IMPLEMENTED);
		return FALSE;
	}
	if (length < sizeof(BOOLEAN)) {
		setLastError(ERROR_BAD_LENGTH);
		return FALSE;
	}
	if (!information) {
		setLastError(ERROR_NOACCESS);
		return FALSE;
	}
	HandleMeta metadata{};
	auto object = wibo::handles().getAs<FsObject>(file, &metadata);
	if (!object || !object->valid()) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & DELETE)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (!(object->flags & Of_File)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	auto fileObject = object.clone().downcast<FileObject>();
	if (fileObject->isPipe) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BOOLEAN requested{};
	std::memcpy(&requested, information, sizeof(requested));
	DWORD error;
	{
		std::lock_guard lock(fileObject->m);
		error = files::setDeleteDispositionLocked(*fileObject, requested != 0);
	}
	if (error != ERROR_SUCCESS) {
		setLastError(error);
		return FALSE;
	}
	setLastError(incomingError);
	return TRUE;
}

} // namespace kernel32
