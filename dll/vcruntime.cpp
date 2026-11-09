#include "vcruntime.h"

#include "common.h"
#include "context.h"
#include "heap.h"
#include "modules.h"

#include <cstring>
#include <limits>

namespace vcruntime {

void CDECL __std_exception_copy(const ExceptionData *source, ExceptionData *destination) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("__std_exception_copy(%p, %p)\n", static_cast<const void *>(source), static_cast<void *>(destination));
	ExceptionData result{};
	if (source->what != GUEST_NULL) {
		if (source->doFree) {
			const char *text = fromGuestPtr<const char>(source->what);
			const size_t length = std::strlen(text);
			if (length != std::numeric_limits<size_t>::max()) {
				auto *message = static_cast<char *>(wibo::heap::guestMalloc(length + 1));
				if (message != nullptr) {
					std::memcpy(message, text, length + 1);
					result = {toGuestPtr(message), true};
				}
			}
		} else {
			result.what = source->what;
		}
	}
	*destination = result;
}

void CDECL __std_exception_destroy(ExceptionData *data) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("__std_exception_destroy(%p)\n", static_cast<void *>(data));
	if (data->doFree) {
		wibo::heap::guestFree(fromGuestPtr<void>(data->what));
	}
	*data = {};
}

PVOID CDECL memchr(LPCVOID buffer, int ch, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("memchr(%p, %d, %zu)\n", buffer, ch, count);
	return const_cast<PVOID>(::memchr(buffer, ch, count));
}

LPSTR CDECL strstr(LPCSTR text, LPCSTR needle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("strstr(%p, %p)\n", static_cast<const void *>(text), static_cast<const void *>(needle));
	return const_cast<LPSTR>(::strstr(text, needle));
}

LPSTR CDECL strrchr(LPCSTR text, int ch) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("strrchr(%p, %d)\n", static_cast<const void *>(text), ch);
	return const_cast<LPSTR>(::strrchr(text, ch));
}

LPSTR CDECL strchr(LPCSTR text, int ch) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("strchr(%p, %d)\n", static_cast<const void *>(text), ch);
	return const_cast<LPSTR>(::strchr(text, ch));
}

PVOID CDECL memcpy(PVOID dest, LPCVOID src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return ::memcpy(dest, src, count);
}

PVOID CDECL memset(PVOID dest, int ch, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return ::memset(dest, ch, count);
}

int CDECL memcmp(LPCVOID buf1, LPCVOID buf2, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return ::memcmp(buf1, buf2, count);
}

PVOID CDECL memmove(PVOID dest, LPCVOID src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return ::memmove(dest, src, count);
}

} // namespace vcruntime

#include "vcruntime_trampolines.h"

extern const wibo::ModuleStub lib_vcruntime = {
	(const char *[]){
		"vcruntime140",
		nullptr,
	},
	vcruntimeThunkByName,
	nullptr,
};
