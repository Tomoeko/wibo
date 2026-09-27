#include "test_assert.h"
#include <windows.h>

#include <evntprov.h>

typedef ULONG(WINAPI *EventSetInformationFn)(REGHANDLE, EVENT_INFO_CLASS, PVOID, ULONG);

enum {
	provider_binary_track = 0,
	provider_reserved1 = 1,
	provider_traits = 2,
	provider_descriptor_type = 3,
	provider_reserved2 = 4,
};

_Static_assert(sizeof(REGHANDLE) == 8, "Registration handles have a fixed width");
_Static_assert(sizeof(EVENT_INFO_CLASS) == 4, "Information classes have a fixed width");

static unsigned callback_count;
static EventSetInformationFn set_information;

static ULONG information_result(const char *name, REGHANDLE handle, unsigned information_class, PVOID buffer,
								ULONG length) {
	SetLastError(0x4321);
	const ULONG result = set_information(handle, (EVENT_INFO_CLASS)information_class, buffer, length);
	const DWORD last_error = GetLastError();
	printf("information %s: class=%u length=%lu status=%lu last_error=%lu\n", name, information_class,
		   (unsigned long)length, (unsigned long)result, (unsigned long)last_error);
	TEST_CHECK_EQ(0x4321, last_error);
	return result;
}

static void expect_information(const char *name, REGHANDLE handle, unsigned information_class, PVOID buffer,
							   ULONG length, ULONG expected) {
	const ULONG result = information_result(name, handle, information_class, buffer, length);
#ifdef EVENT_INFORMATION_OBSERVE
	(void)expected;
	(void)result;
#else
	TEST_CHECK_EQ(expected, result);
#endif
}

static void test_information(REGHANDLE first, REGHANDLE second) {
	BYTE enabled = TRUE;
	BYTE disabled = FALSE;
	BYTE invalid_boolean = 2;
	BYTE valid_traits[] = {10, 0, 'f', 'i', 'x', 't', 'u', 'r', 'e', 0};
	BYTE valid_traits_copy[sizeof(valid_traits)];
	memcpy(valid_traits_copy, valid_traits, sizeof(valid_traits));
	expect_information("track", first, provider_binary_track, NULL, 0, ERROR_NOT_SUPPORTED);
	expect_information("track-null", first, provider_binary_track, NULL, 1, ERROR_INVALID_PARAMETER);
	expect_information("zero-handle", 0, provider_descriptor_type, &enabled, sizeof(enabled), ERROR_INVALID_PARAMETER);
	expect_information("invalid-handle", ~(REGHANDLE)0, provider_descriptor_type, &enabled, sizeof(enabled),
					   ERROR_INVALID_PARAMETER);
	expect_information("unknown-class", first, 0x7fff, NULL, 0, ERROR_NOT_SUPPORTED);
	expect_information("reserved1", first, provider_reserved1, NULL, 0, ERROR_NOT_SUPPORTED);
	expect_information("reserved2", first, provider_reserved2, NULL, 0, ERROR_NOT_SUPPORTED);
	expect_information("traits-null-empty", first, provider_traits, NULL, 0, ERROR_INVALID_PARAMETER);
	expect_information("traits-null", first, provider_traits, NULL, 1, ERROR_INVALID_PARAMETER);
	expect_information("traits-short", first, provider_traits, valid_traits, 1, ERROR_INVALID_PARAMETER);
	expect_information("traits-header-only", first, provider_traits, valid_traits, 2, ERROR_INVALID_PARAMETER);
	expect_information("traits-length", first, provider_traits, valid_traits, sizeof(valid_traits) - 1,
					   ERROR_INVALID_PARAMETER);
	BYTE unterminated_name[] = {4, 0, 'a', 'b'};
	expect_information("traits-name", first, provider_traits, unterminated_name, sizeof(unterminated_name),
					   ERROR_INVALID_PARAMETER);
	BYTE short_trait[] = {5, 0, 0, 2, 0};
	expect_information("traits-entry-short", first, provider_traits, short_trait, sizeof(short_trait),
					   ERROR_INVALID_PARAMETER);
	BYTE oversized_trait[] = {6, 0, 0, 7, 0, 128};
	expect_information("traits-entry-length", first, provider_traits, oversized_trait, sizeof(oversized_trait),
					   ERROR_INVALID_PARAMETER);
	BYTE invalid_utf8[] = {5, 0, 0xc0, 0xaf, 0};
	expect_information("traits-utf8", first, provider_traits, invalid_utf8, sizeof(invalid_utf8),
					   ERROR_INVALID_PARAMETER);
	BYTE custom_trait[] = {7, 0, 0, 4, 0, 128, 0x42};
	expect_information("traits-custom-entry", first, provider_traits, custom_trait, sizeof(custom_trait),
					   ERROR_NOT_SUPPORTED);
	_Alignas(2) BYTE unaligned_traits[] = {0, 3, 0, 0};
	expect_information("traits-unaligned", first, provider_traits, unaligned_traits + 1, sizeof(unaligned_traits) - 1,
					   ERROR_NOT_SUPPORTED);
	expect_information("traits-first", first, provider_traits, valid_traits, sizeof(valid_traits), ERROR_NOT_SUPPORTED);
	TEST_CHECK(memcmp(valid_traits_copy, valid_traits, sizeof(valid_traits)) == 0);
	expect_information("traits-repeat", first, provider_traits, valid_traits, sizeof(valid_traits),
					   ERROR_NOT_SUPPORTED);
	expect_information("traits-independent", second, provider_traits, valid_traits, sizeof(valid_traits),
					   ERROR_NOT_SUPPORTED);
	expect_information("descriptor-enable", first, provider_descriptor_type, &enabled, sizeof(enabled), ERROR_SUCCESS);
	expect_information("descriptor-disable", first, provider_descriptor_type, &disabled, sizeof(disabled),
					   ERROR_SUCCESS);
	expect_information("descriptor-null", first, provider_descriptor_type, NULL, 1, ERROR_INVALID_PARAMETER);
	expect_information("descriptor-empty", first, provider_descriptor_type, &enabled, 0, ERROR_INVALID_PARAMETER);
	expect_information("descriptor-length", first, provider_descriptor_type, valid_traits, 2, ERROR_INVALID_PARAMETER);
	expect_information("descriptor-boolean", first, provider_descriptor_type, &invalid_boolean, 1,
					   ERROR_INVALID_PARAMETER);
	TEST_CHECK_EQ(TRUE, enabled);
	TEST_CHECK_EQ(FALSE, disabled);
}

static void WINAPI disabled_callback(LPCGUID id, ULONG enabled, UCHAR level, ULONGLONG any_keyword,
									 ULONGLONG all_keyword, PEVENT_FILTER_DESCRIPTOR filter, PVOID context) {
	(void)id;
	(void)enabled;
	(void)level;
	(void)any_keyword;
	(void)all_keyword;
	(void)filter;
	(void)context;
	++callback_count;
}

int main(void) {
	HMODULE module = GetModuleHandleA("advapi32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "EventSetInformation");
	_Static_assert(sizeof(exported) == sizeof(set_information), "Resolved function pointers have the same width");
	memcpy(&set_information, &exported, sizeof(set_information));
	TEST_CHECK(set_information != NULL);
	GUID id = {0x1681d246, 0x5357, 0x4944, {0x91, 0x52, 0x40, 0x31, 0x25, 0x16, 0x73, 0x02}};
	id.Data1 ^= GetCurrentProcessId();
	struct {
		ULONGLONG before;
		REGHANDLE handle;
		ULONGLONG after;
	} first = {0x1122334455667788ULL, 0, 0x1122334455667788ULL};
	SetLastError(0x4321);
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&id, disabled_callback, NULL, &first.handle));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(first.handle != 0);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, first.before);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, first.after);
	REGHANDLE second = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&id, NULL, NULL, &second));
	TEST_CHECK(second != 0);
	printf("registration handles: first=%llx second=%llx distinct=%u\n", (unsigned long long)first.handle,
		   (unsigned long long)second, first.handle != second);
#ifndef EVENT_INFORMATION_OBSERVE
	TEST_CHECK(second != first.handle);
#endif
	test_information(first.handle, second);
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(first.handle));
	BYTE enabled = TRUE;
	expect_information("descriptor-retired", first.handle, provider_descriptor_type, &enabled, sizeof(enabled),
					   ERROR_INVALID_PARAMETER);
	BYTE replacement_traits[] = {3, 0, 0};
	expect_information("second-survives", second, provider_descriptor_type, replacement_traits + 2, 1, ERROR_SUCCESS);
	REGHANDLE replacement = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&id, NULL, NULL, &replacement));
	TEST_CHECK(replacement != 0);
#ifndef EVENT_INFORMATION_OBSERVE
	TEST_CHECK(replacement != first.handle && replacement != second);
#endif
	expect_information("descriptor-new-registration", replacement, provider_descriptor_type, &enabled, sizeof(enabled),
					   ERROR_SUCCESS);
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(second));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(replacement));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(0));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK_EQ(0, callback_count);
	return 0;
}
