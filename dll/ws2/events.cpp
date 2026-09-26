#include "ws2.h"

#include "common.h"
#include "context.h"
#include "handles.h"
#include "kernel32/handleapi.h"
#include "kernel32/internal.h"
#include "kernel32/synchapi.h"
#include "ws2/internal.h"

namespace ws2 {
HANDLE WINAPI WSACreateEvent() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSACreateEvent()\n");
	if (!detail::requireStarted())
		return 0;
	const HANDLE result = kernel32::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!result)
		detail::setLastError(static_cast<int>(kernel32::getLastError()));
	return result;
}
BOOL WINAPI WSACloseEvent(HANDLE event) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSACloseEvent(%p)\n", event);
	if (!detail::requireStarted())
		return FALSE;
	if (!wibo::handles().getAs<kernel32::EventObject>(event)) {
		detail::setLastError(6);
		return FALSE;
	}
	const BOOL result = kernel32::CloseHandle(event);
	if (!result)
		detail::setLastError(static_cast<int>(kernel32::getLastError()));
	return result;
}
BOOL WINAPI WSASetEvent(HANDLE event) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSASetEvent(%p)\n", event);
	if (!detail::requireStarted())
		return FALSE;
	const BOOL result = kernel32::SetEvent(event);
	if (!result)
		detail::setLastError(static_cast<int>(kernel32::getLastError()));
	return result;
}
BOOL WINAPI WSAResetEvent(HANDLE event) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAResetEvent(%p)\n", event);
	if (!detail::requireStarted())
		return FALSE;
	const BOOL result = kernel32::ResetEvent(event);
	if (!result)
		detail::setLastError(static_cast<int>(kernel32::getLastError()));
	return result;
}
DWORD WINAPI WSAWaitForMultipleEvents(DWORD count, const HANDLE *events, BOOL waitAll, DWORD timeout, BOOL alertable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAWaitForMultipleEvents(%u, %p, %d, %u, %d)\n", count, events, waitAll, timeout, alertable);
	if (!detail::requireStarted())
		return 0xFFFFFFFF;
	if (!count || count > 64 || !events) {
		detail::setLastError(10022);
		return 0xFFFFFFFF;
	}
	if (alertable) {
		detail::setLastError(10045);
		return 0xFFFFFFFF;
	}
	const DWORD result = kernel32::WaitForMultipleObjects(count, events, waitAll, timeout);
	if (result == 0xFFFFFFFF) {
		const DWORD error = kernel32::getLastError();
		detail::setLastError(error == 87 ? 10022 : static_cast<int>(error));
	}
	return result;
}
} // namespace ws2
