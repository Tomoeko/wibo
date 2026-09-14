// evntrace.h requires the Windows base types first.
#include <windows.h>

#include <evntrace.h>
#include <stddef.h>

#include "test_assert.h"

// Strict registration contract, not an active ETW tracing test. Wine 11's A
// implementation returns success without handles and its W implementation uses
// a fixed dummy handle. This fixture intentionally fails on that incomplete
// lifecycle instead of accepting it as a correctness reference.
// https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-registertraceguidsa
// https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-unregistertraceguids
_Static_assert(sizeof(TRACEHANDLE) == 8, "provider handles are always 64 bit");
_Static_assert(sizeof(TRACE_GUID_REGISTRATION) == 2 * sizeof(void *), "class registration stride");
_Static_assert(offsetof(TRACE_GUID_REGISTRATION, RegHandle) == sizeof(void *), "class handle offset");

static const DWORD kSentinelError = 0x12345678;
static const ULONGLONG kGuard = 0x1122334455667788ULL;
static unsigned callbackCount;
static GUID controlGuid = {0x31fdc2e7, 0x0b52, 0x4b51, {0xac, 0x8f, 0x3b, 0x49, 0x62, 0x7c, 0xda, 0x01}};
static const GUID classGuids[] = {
	{0x721e8bc0, 0xf490, 0x478a, {0x9d, 0xd3, 0xa1, 0x1b, 0x5d, 0x1e, 0xf3, 0x50}},
	{0xcafe940e, 0x859f, 0x4d4c, {0x81, 0x71, 0x4c, 0xef, 0x52, 0x9a, 0x19, 0x39}},
};

static ULONG WINAPI disabled_callback(WMIDPREQUESTCODE request, PVOID context, ULONG *size, PVOID buffer) {
	(void)request;
	(void)context;
	(void)size;
	(void)buffer;
	++callbackCount;
	return ERROR_CANCELLED;
}

static void check_unregister(TRACEHANDLE handle, ULONG expected) {
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(expected, UnregisterTraceGuids(handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(0, callbackCount);
}

static void test_lifecycle(void) {
	struct {
		ULONGLONG before;
		TRACEHANDLE handle;
		ULONGLONG after;
	} first = {kGuard, 0, kGuard}, second = {kGuard, 0, kGuard};
	struct {
		ULONG_PTR before;
		TRACE_GUID_REGISTRATION entries[2];
		ULONG_PTR after;
	} classes = {(ULONG_PTR)kSentinelError, {{&classGuids[0], 0}, {&classGuids[1], 0}}, (ULONG_PTR)kSentinelError};
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(ERROR_SUCCESS, RegisterTraceGuidsA(disabled_callback, &callbackCount, &controlGuid, 2,
													 classes.entries, NULL, NULL, &first.handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_MSG(first.handle != 0, "registration succeeded without providing a provider handle");
	TEST_CHECK_U64_EQ(kGuard, first.before);
	TEST_CHECK_U64_EQ(kGuard, first.after);
	TEST_CHECK_EQ(kSentinelError, classes.before);
	TEST_CHECK_EQ(kSentinelError, classes.after);
	TEST_CHECK(classes.entries[0].Guid == &classGuids[0]);
	TEST_CHECK(classes.entries[1].Guid == &classGuids[1]);
	TEST_CHECK(classes.entries[0].RegHandle != NULL);
	TEST_CHECK(classes.entries[1].RegHandle != NULL);
	TEST_CHECK(classes.entries[0].RegHandle != classes.entries[1].RegHandle);
	TEST_CHECK_EQ(0, callbackCount);
	TRACE_GUID_REGISTRATION more = {&classGuids[0], 0};
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegisterTraceGuidsW(disabled_callback, NULL, &controlGuid, 1, &more, NULL, NULL, &second.handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK(second.handle != 0 && second.handle != first.handle);
	TEST_CHECK(more.RegHandle != NULL && more.RegHandle != classes.entries[0].RegHandle);
	TEST_CHECK_U64_EQ(kGuard, second.before);
	TEST_CHECK_U64_EQ(kGuard, second.after);
	// The upper DWORD participates in lookup even for a 32-bit guest.
	check_unregister(first.handle ^ 0x8000000000000000ULL, ERROR_INVALID_PARAMETER);
	check_unregister((TRACEHANDLE)(ULONG_PTR)classes.entries[0].RegHandle, ERROR_INVALID_PARAMETER);
	check_unregister(first.handle, ERROR_SUCCESS);
	check_unregister(first.handle, ERROR_INVALID_PARAMETER);
	check_unregister(second.handle, ERROR_SUCCESS);
	check_unregister(second.handle, ERROR_INVALID_PARAMETER);
	check_unregister(0, ERROR_INVALID_PARAMETER);
}

static void test_zero_classes(void) {
	TRACEHANDLE handle = 0;
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegisterTraceGuidsA(disabled_callback, NULL, &controlGuid, 0, NULL, NULL, NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK(handle != 0);
	check_unregister(handle, ERROR_SUCCESS);
	TRACE_GUID_REGISTRATION ignored = {&classGuids[0], (HANDLE)(ULONG_PTR)0x12345678};
	handle = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS,
				  RegisterTraceGuidsW(disabled_callback, NULL, &controlGuid, 0, &ignored, NULL, NULL, &handle));
	TEST_CHECK(handle != 0);
	TEST_CHECK_EQ(0x12345678, (ULONG_PTR)ignored.RegHandle);
	check_unregister(handle, ERROR_SUCCESS);
}

static void test_invalid_registration(void) {
	TRACE_GUID_REGISTRATION entry = {&classGuids[0], 0};
	TRACEHANDLE handle = 0;
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  RegisterTraceGuidsA(NULL, NULL, &controlGuid, 1, &entry, NULL, NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  RegisterTraceGuidsW(disabled_callback, NULL, NULL, 1, &entry, NULL, NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  RegisterTraceGuidsA(disabled_callback, NULL, &controlGuid, 1, &entry, NULL, NULL, NULL));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  RegisterTraceGuidsW(disabled_callback, NULL, &controlGuid, 1, NULL, NULL, NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	entry.Guid = NULL;
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  RegisterTraceGuidsA(disabled_callback, NULL, &controlGuid, 1, &entry, NULL, NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(0, callbackCount);
}

static void test_unsupported_mof(void) {
	// This is an explicit wibo limitation, not a Windows MOF compatibility claim.
	TRACEHANDLE handle = 0;
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED,
				  RegisterTraceGuidsA(disabled_callback, NULL, &controlGuid, 0, NULL, "unused.dll", NULL, &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED,
				  RegisterTraceGuidsW(disabled_callback, NULL, &controlGuid, 0, NULL, NULL, L"unused", &handle));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(0, callbackCount);
}

int main(void) {
	controlGuid.Data1 ^= GetCurrentProcessId();
	test_lifecycle();
	test_zero_classes();
	test_invalid_registration();
	test_unsupported_mof();
	puts("disabled trace registration tests passed");
	return 0;
}
