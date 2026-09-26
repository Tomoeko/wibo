#include "handleapi.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "processthreadsapi.h"
#include "ws2/internal.h"

#include <pthread.h>
#include <unistd.h>

namespace {

bool isUserImage(HANDLE handle) {
	const auto object = wibo::handles().get(handle);
	return object && object->type == ObjectType::UserImage;
}

} // namespace

namespace kernel32 {

BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
							LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DuplicateHandle(%p, %p, %p, %p, %x, %d, %x)\n", hSourceProcessHandle, hSourceHandle,
			  hTargetProcessHandle, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
	(void)dwDesiredAccess;
	(void)dwOptions;
	if (!lpTargetHandle) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	auto validateProcessHandle = [&](HANDLE handle) -> bool {
		if (isPseudoCurrentProcessHandle(handle)) {
			return true;
		}
		auto proc = wibo::handles().getAs<ProcessObject>(handle);
		return proc && proc->pid == getpid();
	};

	if (!validateProcessHandle(hSourceProcessHandle) || !validateProcessHandle(hTargetProcessHandle)) {
		DEBUG_LOG("DuplicateHandle: unsupported process handle combination (source=%p target=%p)\n",
				  hSourceProcessHandle, hTargetProcessHandle);
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	auto &handles = wibo::handles();
	if (isPseudoCurrentProcessHandle(hSourceHandle)) {
		auto po = make_pin<ProcessObject>(getpid(), -1, false);
		auto handle = handles.alloc(std::move(po), 0, 0);
		DEBUG_LOG("DuplicateHandle: created process handle for current process -> %p\n", handle);
		*lpTargetHandle = handle;
		return TRUE;
	} else if (isPseudoCurrentThreadHandle(hSourceHandle)) {
		auto th = currentThreadObject();
		const auto access = (dwOptions & DUPLICATE_SAME_ACCESS) ? THREAD_ALL_ACCESS : dwDesiredAccess;
		auto handle = handles.alloc(std::move(th), access, bInheritHandle ? HANDLE_FLAG_INHERIT : 0);
		DEBUG_LOG("DuplicateHandle: created thread handle for current thread -> %p\n", handle);
		*lpTargetHandle = handle;
		return TRUE;
	}

	if (isUserImage(hSourceHandle) ||
		!handles.duplicateTo(hSourceHandle, handles, *lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions)) {
		DEBUG_LOG("-> ERROR_INVALID_HANDLE\n");
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	DEBUG_LOG("-> %p\n", *lpTargetHandle);
	return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE hObject) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CloseHandle(%p)\n", hObject);
	if (isUserImage(hObject) || !wibo::handles().release(hObject)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetHandleInformation(HANDLE handle, LPDWORD flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetHandleInformation(%p, %p)\n", handle, flags);
	if (!flags) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (isUserImage(handle) || (!wibo::handles().getInformation(handle, flags) &&
								!ws2::detail::getHandleInformation(static_cast<SOCKET>(handle), flags))) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetHandleInformation(%p, 0x%x, 0x%x)\n", hObject, dwMask, dwFlags);
	if (isUserImage(hObject) || (!wibo::handles().setInformation(hObject, dwMask, dwFlags) &&
								 !ws2::detail::setHandleInformation(static_cast<SOCKET>(hObject), dwMask, dwFlags))) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

} // namespace kernel32
