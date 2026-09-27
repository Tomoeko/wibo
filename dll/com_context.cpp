#include "com_context.h"

#include "com_context_trampolines.h"
#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {
constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003);
constexpr HRESULT kNoInterface = static_cast<HRESULT>(0x80004002);
constexpr HRESULT kNotInitialized = static_cast<HRESULT>(0x800401f0);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000e);
constexpr HRESULT kNotImplemented = static_cast<HRESULT>(0x80004001);
constexpr GUID kUnknown{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kObjectContext{0x1c6, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

struct ContextState {
	ULONG references = 0;
	bool threadOwned = true;
};
struct Registry {
	std::mutex mutex;
	std::unordered_map<GUEST_PTR, ContextState> contexts;
	size_t multithreadedApartments = 0;
	GUEST_PTR table = 0;
	~Registry() {
		for (const auto &[token, state] : contexts)
			wibo::heap::guestFree(fromGuestPtr(token));
		wibo::heap::guestFree(fromGuestPtr(table));
	}
};
Registry &registry() {
	static Registry value;
	return value;
}

void eraseUnreferenced(Registry &value, GUEST_PTR token) {
	auto found = value.contexts.find(token);
	if (found != value.contexts.end() && !found->second.threadOwned && !found->second.references) {
		value.contexts.erase(found);
		wibo::heap::guestFree(fromGuestPtr(token));
	}
}

struct ThreadContext;
void releaseThreadContext(ThreadContext &thread);

struct ThreadContext {
	bool initialized = false;
	bool multithreaded = false;
	GUEST_PTR token = 0;
	~ThreadContext() { releaseThreadContext(*this); }
};

void releaseThreadContext(ThreadContext &thread) {
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	if (thread.initialized && thread.multithreaded)
		--value.multithreadedApartments;
	thread.initialized = false;
	if (auto found = value.contexts.find(thread.token); found != value.contexts.end()) {
		found->second.threadOwned = false;
		eraseUnreferenced(value, thread.token);
	}
	thread.token = 0;
}

ThreadContext &threadContext() {
	// Construct the process registry before its thread-local owners.
	(void)registry();
	thread_local ThreadContext value;
	return value;
}

GUEST_PTR makeContext(Registry &value) {
	if (!value.table) {
		constexpr const char *names[] = {"QueryInterface", "AddRef",	  "Release",		  "SetProperty",
										 "RemoveProperty", "GetProperty", "EnumContextProps", "Reserved",
										 "Reserved",	   "Reserved",	  "Reserved",		  "Reserved",
										 "Reserved",	   "Reserved"};
		auto *table =
			static_cast<GUEST_PTR *>(wibo::heap::guestMalloc(sizeof(names) / sizeof(names[0]) * sizeof(GUEST_PTR)));
		if (!table)
			return 0;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
			table[i] = toGuestPtr(com_contextThunkByName(names[i]));
		value.table = toGuestPtr(table);
	}
	auto *object = static_cast<GUEST_PTR *>(wibo::heap::guestMalloc(sizeof(GUEST_PTR)));
	if (!object)
		return 0;
	*object = value.table;
	const auto token = toGuestPtr(object);
	value.contexts.emplace(token, ContextState{});
	return token;
}
} // namespace

namespace wibo::com {
void beginApartment(bool multithreaded) {
	auto &thread = threadContext();
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	thread.initialized = true;
	thread.multithreaded = multithreaded;
	if (multithreaded)
		++value.multithreadedApartments;
}

void endApartment() {
	auto &thread = threadContext();
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	if (thread.initialized && thread.multithreaded)
		--value.multithreadedApartments;
	thread.initialized = false;
}

void releaseThreadContext() { ::releaseThreadContext(threadContext()); }

HRESULT currentContextToken(ULONG_PTR *token) {
	auto &thread = threadContext();
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	if (!thread.initialized && !value.multithreadedApartments)
		return kNotInitialized;
	if (!token)
		return kPointer;
	if (!thread.token)
		thread.token = makeContext(value);
	if (!thread.token)
		return kOutOfMemory;
	*token = thread.token;
	return S_OK;
}
} // namespace wibo::com

namespace com_context {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext::QueryInterface(%p, %p, %p)\n", fromGuestPtr(self), iid, result);
	if (!result)
		return kPointer;
	*result = 0;
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	auto found = value.contexts.find(self);
	if (!iid || found == value.contexts.end())
		return kPointer;
	if (std::memcmp(iid, &kUnknown, sizeof(GUID)) != 0 && std::memcmp(iid, &kObjectContext, sizeof(GUID)) != 0)
		return kNoInterface;
	++found->second.references;
	*result = self;
	return S_OK;
}
ULONG WINAPI AddRef(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	auto found = value.contexts.find(self);
	return found == value.contexts.end() ? 0 : ++found->second.references;
}
ULONG WINAPI Release(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto &value = registry();
	std::lock_guard lock(value.mutex);
	auto found = value.contexts.find(self);
	if (found == value.contexts.end() || !found->second.references)
		return 0;
	const auto count = --found->second.references;
	eraseUnreferenced(value, self);
	return count;
}
HRESULT WINAPI SetProperty(GUEST_PTR, const GUID *, DWORD, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext::SetProperty: unsupported\n");
	return kNotImplemented;
}
HRESULT WINAPI RemoveProperty(GUEST_PTR, const GUID *) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext::RemoveProperty: unsupported\n");
	return kNotImplemented;
}
HRESULT WINAPI GetProperty(GUEST_PTR, const GUID *, DWORD *, GUEST_PTR *) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext::GetProperty: unsupported\n");
	return kNotImplemented;
}
HRESULT WINAPI EnumContextProps(GUEST_PTR, GUEST_PTR *) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext::EnumContextProps: unsupported\n");
	return kNotImplemented;
}
void WINAPI Reserved(GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IObjContext: reserved entry\n");
}
} // namespace com_context
