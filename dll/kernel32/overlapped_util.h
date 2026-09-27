#pragma once

#include "completion_port.h"
#include "handles.h"
#include "internal.h"
#include "minwinbase.h"

namespace kernel32::detail {

inline NTSTATUS loadOverlappedStatus(const OVERLAPPED *ov) {
	return static_cast<NTSTATUS>(__atomic_load_n(&ov->Internal, __ATOMIC_ACQUIRE));
}
inline ULONG_PTR loadOverlappedBytes(const OVERLAPPED *ov) {
	return __atomic_load_n(&ov->InternalHigh, __ATOMIC_RELAXED);
}

inline HANDLE normalizedOverlappedEventHandle(const OVERLAPPED *ov) {
	if (!ov) {
		return NO_HANDLE;
	}
	return ov->hEvent & ~HANDLE{1};
}

inline void storeOverlappedResult(OVERLAPPED &operation, NTSTATUS status, size_t bytesTransferred) {
	__atomic_store_n(&operation.InternalHigh, static_cast<ULONG_PTR>(bytesTransferred), __ATOMIC_RELAXED);
	__atomic_store_n(&operation.Internal, static_cast<ULONG_PTR>(static_cast<DWORD>(status)), __ATOMIC_RELEASE);
}

inline void signalOverlappedCompletion(const std::shared_ptr<const CompletionBinding> &binding, OVERLAPPED *ov,
									   NTSTATUS status, size_t bytesTransferred) {
	const bool postCompletion = binding && ov && !(ov->hEvent & 1U);
	const auto context = toGuestPtr(ov);
	const HANDLE eventHandle = normalizedOverlappedEventHandle(ov);
	if (ov)
		storeOverlappedResult(*ov, status, bytesTransferred);
	if (eventHandle) {
		if (auto ev = wibo::handles().getAs<EventObject>(eventHandle)) {
			ev->set();
		}
	}
	if (postCompletion)
		binding->port->post({static_cast<DWORD>(bytesTransferred), binding->key, context, status});
}

inline void signalOverlappedEvent(FsObject *file, OVERLAPPED *ov, NTSTATUS status, size_t bytesTransferred) {
	const auto binding = file ? std::atomic_load(&file->completion) : nullptr;
	signalOverlappedCompletion(binding, ov, status, bytesTransferred);
	if (file && ov) {
		// Synchronize notification with the waiter entering its condition-variable wait.
		std::lock_guard lock(file->overlappedMutex);
		file->overlappedCv.notify_all();
	}
}

inline void resetOverlappedEvent(OVERLAPPED *ov) {
	if (HANDLE handle = normalizedOverlappedEventHandle(ov)) {
		if (auto ev = wibo::handles().getAs<EventObject>(handle)) {
			ev->reset();
		}
	}
}

} // namespace kernel32::detail
