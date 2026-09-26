#pragma once

#include "completion_port.h"
#include "handles.h"
#include "internal.h"
#include "minwinbase.h"

namespace kernel32::detail {

inline HANDLE normalizedOverlappedEventHandle(const OVERLAPPED *ov) {
	if (!ov) {
		return NO_HANDLE;
	}
	return ov->hEvent & ~HANDLE{1};
}

inline void signalOverlappedEvent(FileObject *file, OVERLAPPED *ov, NTSTATUS status, size_t bytesTransferred) {
	const auto binding = file ? std::atomic_load(&file->completion) : nullptr;
	const bool postCompletion = binding && ov && !(ov->hEvent & 1U);
	const auto context = toGuestPtr(ov);
	const HANDLE eventHandle = normalizedOverlappedEventHandle(ov);
	if (ov) {
		ov->Internal = status;
		ov->InternalHigh = static_cast<ULONG_PTR>(bytesTransferred);
	}
	if (eventHandle) {
		if (auto ev = wibo::handles().getAs<EventObject>(eventHandle)) {
			ev->set();
		}
	}
	if (file) {
		file->overlappedCv.notify_all();
	}
	if (postCompletion)
		binding->port->post({static_cast<DWORD>(bytesTransferred), binding->key, context, status});
}

inline void resetOverlappedEvent(OVERLAPPED *ov) {
	if (HANDLE handle = normalizedOverlappedEventHandle(ov)) {
		if (auto ev = wibo::handles().getAs<EventObject>(handle)) {
			ev->reset();
		}
	}
}

} // namespace kernel32::detail
