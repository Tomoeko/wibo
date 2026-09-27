#include "thread_description.h"

#include "context.h"
#include "errors.h"
#include "heap.h"
#include "internal.h"
#include "processthreadsapi.h"
#include "strutil.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr size_t kMaxDescriptionUnits = std::numeric_limits<USHORT>::max() / sizeof(WCHAR);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xc0000017u);

HRESULT resultFromStatus(NTSTATUS status) { return static_cast<HRESULT>(static_cast<uint32_t>(status) | 0x10000000u); }

struct PreserveLastError {
	DWORD value = kernel32::getLastError();
	~PreserveLastError() { kernel32::setLastError(value); }
};

Pin<kernel32::ThreadObject> descriptionThread(HANDLE handle, DWORD access, NTSTATUS &status) {
	if (kernel32::isPseudoCurrentThreadHandle(handle)) {
		auto thread = kernel32::currentThreadObject();
		status = thread ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
		return thread;
	}
	if (kernel32::isPseudoCurrentProcessHandle(handle)) {
		status = STATUS_OBJECT_TYPE_MISMATCH;
		return {};
	}
	HandleMeta metadata{};
	auto object = wibo::handles().get(handle, &metadata);
	if (!object) {
		status = STATUS_INVALID_HANDLE;
		return {};
	}
	if (object->type != ObjectType::Thread && object->type != ObjectType::ProcessThread) {
		status = STATUS_OBJECT_TYPE_MISMATCH;
		return {};
	}
	if (!(metadata.grantedAccess & access)) {
		status = STATUS_ACCESS_DENIED;
		return {};
	}
	// Remote primary-thread proxies do not share description state with their runtime.
	if (object->type == ObjectType::ProcessThread) {
		status = STATUS_NOT_SUPPORTED;
		return {};
	}
	status = STATUS_SUCCESS;
	return std::move(object).downcast<kernel32::ThreadObject>();
}

void mirrorHostDescription(const kernel32::ThreadObject &thread) {
#if defined(__APPLE__) || defined(__linux__)
	if (!thread.initialized || thread.signaled || thread.thread == kernel32::pthread_null)
		return;
#if defined(__APPLE__)
	// The host naming API can rename only the calling thread.
	if (!pthread_equal(thread.thread, pthread_self()))
		return;
	constexpr size_t kHostNameBytes = 63;
#else
	constexpr size_t kHostNameBytes = 15;
#endif
	// Host labels are bounded independently of the complete guest UTF-16 description.
	std::array<char16_t, kHostNameBytes> prefix{};
	size_t units = std::min(thread.descriptionLength, prefix.size());
	for (size_t index = 0; index < units; ++index)
		prefix[index] = thread.description[index];
	if (units < thread.descriptionLength && units && prefix[units - 1] >= 0xd800 && prefix[units - 1] <= 0xdbff)
		--units;
	std::string name;
	if (!utf16ToUtf8(std::u16string_view(prefix.data(), units), name))
		return;
	if (name.size() > kHostNameBytes) {
		size_t boundary = kHostNameBytes;
		while (boundary && (static_cast<unsigned char>(name[boundary]) & 0xc0) == 0x80)
			--boundary;
		name.resize(boundary);
	}
#if defined(__APPLE__)
	const int error = pthread_setname_np(name.c_str());
#else
	const int error = pthread_setname_np(thread.thread, name.c_str());
#endif
	if (error)
		DEBUG_LOG("Host thread description was not applied: error=%d\n", error);
#else
	(void)thread;
#endif
}
} // namespace

namespace kernel32 {

HRESULT WINAPI SetThreadDescription(HANDLE handle, LPCWSTR description) {
	HOST_CONTEXT_GUARD();
	PreserveLastError preserve;
	DEBUG_LOG("SetThreadDescription(%p, %p)\n", handle, description);
	const size_t length = wstrnlen(description, kMaxDescriptionUnits + 1);
	if (length > kMaxDescriptionUnits)
		return resultFromStatus(STATUS_INVALID_PARAMETER);
	NTSTATUS status = STATUS_SUCCESS;
	auto thread = descriptionThread(handle, THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION, status);
	if (!thread)
		return resultFromStatus(status);
	std::unique_ptr<WCHAR[]> snapshot;
	if (length) {
		snapshot.reset(new (std::nothrow) WCHAR[length]);
		if (!snapshot)
			return resultFromStatus(kStatusNoMemory);
		std::memcpy(snapshot.get(), description, length * sizeof(WCHAR));
	}
	std::lock_guard lock(thread->m);
	thread->description = std::move(snapshot);
	thread->descriptionLength = length;
	mirrorHostDescription(*thread);
	return resultFromStatus(STATUS_SUCCESS);
}

HRESULT WINAPI GetThreadDescription(HANDLE handle, GUEST_PTR *description) {
	HOST_CONTEXT_GUARD();
	PreserveLastError preserve;
	DEBUG_LOG("GetThreadDescription(%p, %p)\n", handle, description);
	if (!description)
		return resultFromStatus(STATUS_INVALID_PARAMETER);
	*description = GUEST_NULL;
	NTSTATUS status = STATUS_SUCCESS;
	auto thread = descriptionThread(handle, THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, status);
	if (!thread)
		return resultFromStatus(status);
	std::lock_guard lock(thread->m);
	auto *snapshot = static_cast<WCHAR *>(wibo::heap::guestMalloc((thread->descriptionLength + 1) * sizeof(WCHAR)));
	if (!snapshot)
		return resultFromStatus(kStatusNoMemory);
	if (thread->descriptionLength)
		std::memcpy(snapshot, thread->description.get(), thread->descriptionLength * sizeof(WCHAR));
	snapshot[thread->descriptionLength] = 0;
	*description = toGuestPtr(snapshot);
	return resultFromStatus(STATUS_SUCCESS);
}

} // namespace kernel32
