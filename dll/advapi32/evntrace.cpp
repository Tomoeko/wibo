#include "evntrace.h"

#include "common.h"
#include "context.h"
#include "errors.h"

#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace {

// Registration-only service for disabled classic providers. No controller,
// active logging session, cross-process enablement, or MOF publication exists.
// In particular, registering a provider must not manufacture an enable callback.
// https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-registertraceguidsa
// https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-unregistertraceguids
struct TraceClass {
	GUID guid;
	HANDLE handle;
};

struct TraceProvider {
	TRACEGUID_HANDLE handle;
	GUID controlGuid;
	WMIDPREQUEST requestAddress;
	PVOID requestContext;
	ULONG classCount;
	std::unique_ptr<TraceClass[]> classes;
	std::unique_ptr<TraceProvider> next;
};

struct TraceRegistry {
	std::mutex mutex;
	std::unique_ptr<TraceProvider> providers;
	// Provider handles are full 64-bit opaque values, distinct from the
	// guest-pointer-sized class handles. Never reuse a retired identifier.
	TRACEGUID_HANDLE nextProviderHandle = 0x100000000ULL;
	ULONGLONG nextClassHandle = 1;

	~TraceRegistry() {
		// Release iteratively so shutdown does not recurse through registrations.
		while (providers) {
			auto next = std::move(providers->next);
			providers = std::move(next);
		}
	}
};

TraceRegistry &traceRegistry() {
	static TraceRegistry registry;
	return registry;
}

ULONG registerTraceGuids(WMIDPREQUEST requestAddress, PVOID requestContext, const GUID *controlGuid, ULONG guidCount,
						 TRACE_GUID_REGISTRATION *traceGuidReg, bool hasMofResource,
						 TRACEGUID_HANDLE *registrationHandle) {
	if (!requestAddress || !controlGuid || !registrationHandle || (guidCount != 0 && !traceGuidReg)) {
		return ERROR_INVALID_PARAMETER;
	}
	if (hasMofResource) {
		return ERROR_NOT_SUPPORTED;
	}
	// Zero classes is the documented Vista+ form for TraceEvent-only providers.
	// Positive counts still require an array of class GUID pointers.
	for (ULONG index = 0; index < guidCount; ++index) {
		if (!traceGuidReg[index].Guid) {
			return ERROR_INVALID_PARAMETER;
		}
	}

	auto &registry = traceRegistry();
	std::lock_guard lock(registry.mutex);
	constexpr ULONGLONG kMaxClassHandle = 0x7fffffffULL;
	if (registry.nextProviderHandle == std::numeric_limits<TRACEGUID_HANDLE>::max() ||
		guidCount > kMaxClassHandle - registry.nextClassHandle + 1) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	if (guidCount != 0 && sizeof(TraceClass) > std::numeric_limits<size_t>::max() / guidCount) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	std::unique_ptr<TraceClass[]> classes;
	if (guidCount != 0) {
		classes.reset(new (std::nothrow) TraceClass[guidCount]);
		if (!classes) {
			return ERROR_NOT_ENOUGH_MEMORY;
		}
	}
	for (ULONG index = 0; index < guidCount; ++index) {
		classes[index] = {*traceGuidReg[index].Guid, static_cast<HANDLE>(registry.nextClassHandle + index)};
	}
	TRACEGUID_HANDLE handle = registry.nextProviderHandle;
	auto provider = std::unique_ptr<TraceProvider>(new (std::nothrow) TraceProvider{
		handle, *controlGuid, requestAddress, requestContext, guidCount, std::move(classes), nullptr});
	if (!provider) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	++registry.nextProviderHandle;
	registry.nextClassHandle += guidCount;
	provider->next = std::move(registry.providers);
	registry.providers = std::move(provider);
	for (ULONG index = 0; index < guidCount; ++index) {
		traceGuidReg[index].RegHandle = registry.providers->classes[index].handle;
	}
	*registrationHandle = handle;
	return ERROR_SUCCESS;
}

} // namespace

namespace advapi32 {

ULONG WINAPI RegisterTraceGuidsA(WMIDPREQUEST RequestAddress, PVOID RequestContext, const GUID *ControlGuid,
								 ULONG GuidCount, TRACE_GUID_REGISTRATION *TraceGuidReg, LPCSTR MofImagePath,
								 LPCSTR MofResourceName, TRACEGUID_HANDLE *RegistrationHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterTraceGuidsA(%p, %p, %p, %u, %p, %p, %p, %p)\n", RequestAddress, RequestContext, ControlGuid,
			  GuidCount, TraceGuidReg, MofImagePath, MofResourceName, RegistrationHandle);
	ULONG result = registerTraceGuids(RequestAddress, RequestContext, ControlGuid, GuidCount, TraceGuidReg,
									  MofImagePath || MofResourceName, RegistrationHandle);
	DEBUG_LOG("RegisterTraceGuidsA -> %u (disabled provider)\n", result);
	return result;
}

ULONG WINAPI RegisterTraceGuidsW(WMIDPREQUEST RequestAddress, PVOID RequestContext, const GUID *ControlGuid,
								 ULONG GuidCount, TRACE_GUID_REGISTRATION *TraceGuidReg, LPCWSTR MofImagePath,
								 LPCWSTR MofResourceName, TRACEGUID_HANDLE *RegistrationHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterTraceGuidsW(%p, %p, %p, %u, %p, %p, %p, %p)\n", RequestAddress, RequestContext, ControlGuid,
			  GuidCount, TraceGuidReg, MofImagePath, MofResourceName, RegistrationHandle);
	ULONG result = registerTraceGuids(RequestAddress, RequestContext, ControlGuid, GuidCount, TraceGuidReg,
									  MofImagePath || MofResourceName, RegistrationHandle);
	DEBUG_LOG("RegisterTraceGuidsW -> %u (disabled provider)\n", result);
	return result;
}

ULONG WINAPI UnregisterTraceGuids(TRACEGUID_HANDLE RegistrationHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UnregisterTraceGuids(0x%llx)\n", RegistrationHandle);
	auto &registry = traceRegistry();
	std::lock_guard lock(registry.mutex);
	auto *link = &registry.providers;
	while (*link && (*link)->handle != RegistrationHandle) {
		link = &(*link)->next;
	}
	if (!*link) {
		return ERROR_INVALID_PARAMETER;
	}
	auto retired = std::move(*link);
	*link = std::move(retired->next);
	return ERROR_SUCCESS;
}

} // namespace advapi32
