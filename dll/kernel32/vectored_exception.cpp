#include "vectored_exception.h"

#include "common.h"
#include "context.h"

#include <cstdlib>
#include <mutex>

namespace {

struct VectoredExceptionRegistration {
	PVECTORED_EXCEPTION_HANDLER handler;
	ULONG_PTR token;
	size_t references;
	bool registered;
	VectoredExceptionRegistration *previous;
	VectoredExceptionRegistration *next;
};

std::mutex g_vectoredExceptionMutex;
VectoredExceptionRegistration *g_vectoredExceptionFirst = nullptr;
VectoredExceptionRegistration *g_vectoredExceptionLast = nullptr;
ULONG_PTR g_nextVectoredExceptionToken = 1;
thread_local size_t g_vectoredExceptionTraversalDepth = 0;

struct VectoredExceptionTraversalScope {
	VectoredExceptionTraversalScope() { ++g_vectoredExceptionTraversalDepth; }
	~VectoredExceptionTraversalScope() { --g_vectoredExceptionTraversalDepth; }
};

// All list links and reference counts are protected by the registry mutex.
void releaseVectoredExceptionRegistration(VectoredExceptionRegistration *entry) {
	if (--entry->references != 0) {
		return;
	}
	if (entry->previous) {
		entry->previous->next = entry->next;
	} else {
		g_vectoredExceptionFirst = entry->next;
	}
	if (entry->next) {
		entry->next->previous = entry->previous;
	} else {
		g_vectoredExceptionLast = entry->previous;
	}
	std::free(entry);
}

} // namespace

namespace wibo {

LONG invokeVectoredExceptionHandlers(PEXCEPTION_POINTERS exceptionInfo, VectoredExceptionInvoker invoke) {
	const VectoredExceptionTraversalScope traversal;
	std::unique_lock lock(g_vectoredExceptionMutex);
	auto *entry = g_vectoredExceptionFirst;
	while (entry) {
		++entry->references;
		const auto handler = entry->handler;
		lock.unlock();
		const LONG result = invoke(handler, exceptionInfo);
		lock.lock();
		// Retain the active entry until after advancing through the live list.
		// A self-removal is deferred, so nested invocations can still reach it.
		auto *next = entry->next;
		releaseVectoredExceptionRegistration(entry);
		if (result == EXCEPTION_CONTINUE_EXECUTION) {
			return result;
		}
		entry = next;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

bool hasActiveVectoredExceptionTraversal() noexcept { return g_vectoredExceptionTraversalDepth != 0; }

bool hasRegisteredVectoredExceptionHandlers() {
	std::lock_guard lock(g_vectoredExceptionMutex);
	for (auto *entry = g_vectoredExceptionFirst; entry; entry = entry->next) {
		if (entry->registered)
			return true;
	}
	return false;
}

} // namespace wibo

namespace kernel32 {

PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AddVectoredExceptionHandler(%u, %p)\n", First, Handler);
	auto *entry = static_cast<VectoredExceptionRegistration *>(std::malloc(sizeof(VectoredExceptionRegistration)));
	if (!entry) {
		return nullptr;
	}
	std::unique_lock lock(g_vectoredExceptionMutex);
	if (g_nextVectoredExceptionToken == 0) {
		std::free(entry);
		return nullptr;
	}
	const ULONG_PTR token = g_nextVectoredExceptionToken++;
	*entry = {Handler, token, 1, true, nullptr, nullptr};
	if (First) {
		entry->next = g_vectoredExceptionFirst;
		if (entry->next) {
			entry->next->previous = entry;
		} else {
			g_vectoredExceptionLast = entry;
		}
		g_vectoredExceptionFirst = entry;
	} else {
		entry->previous = g_vectoredExceptionLast;
		if (entry->previous) {
			entry->previous->next = entry;
		} else {
			g_vectoredExceptionFirst = entry;
		}
		g_vectoredExceptionLast = entry;
	}
	// Tokens are never reused, so a stale handle cannot remove a later registration.
	return reinterpret_cast<PVOID>(static_cast<uintptr_t>(token));
}

ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RemoveVectoredExceptionHandler(%p)\n", Handle);
	const auto token = reinterpret_cast<uintptr_t>(Handle);
	std::unique_lock lock(g_vectoredExceptionMutex);
	for (auto *entry = g_vectoredExceptionFirst; entry; entry = entry->next) {
		if (entry->token == token && entry->registered) {
			entry->registered = false;
			releaseVectoredExceptionRegistration(entry);
			return 1;
		}
	}
	return 0;
}

} // namespace kernel32
