#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <stddef.h>

typedef BOOL(WINAPI *GetCPInfoFn)(UINT, DWORD, LPCPINFOEXW);

struct GuardedInfo {
	BYTE before[16];
	CPINFOEXW info;
	BYTE after[16];
};

struct CallResult {
	BOOL success;
	DWORD error;
};

_Static_assert(sizeof(CPINFOEXW) == 544, "The code-page information has the Windows layout");
_Static_assert(_Alignof(CPINFOEXW) == 4, "The code-page information has four-byte alignment");
_Static_assert(offsetof(CPINFOEXW, DefaultChar) == 4, "Default bytes follow the character size");
_Static_assert(offsetof(CPINFOEXW, LeadByte) == 6, "Lead ranges follow the default bytes");
_Static_assert(offsetof(CPINFOEXW, UnicodeDefaultChar) == 18, "The Unicode default follows the lead ranges");
_Static_assert(offsetof(CPINFOEXW, CodePage) == 20, "The resolved identifier follows the Unicode default");
_Static_assert(offsetof(CPINFOEXW, CodePageName) == 24, "The fixed name follows the resolved identifier");

static GetCPInfoFn get_cp_info;

static void reset_info(struct GuardedInfo *output) { memset(output, 0xa5, sizeof(*output)); }

static void check_bounds(const struct GuardedInfo *output) {
	for (unsigned index = 0; index < sizeof(output->before); ++index) {
		TEST_CHECK_EQ(0xa5, output->before[index]);
		TEST_CHECK_EQ(0xa5, output->after[index]);
	}
}

static void check_unchanged(const struct GuardedInfo *output) {
	check_bounds(output);
	const BYTE *bytes = (const BYTE *)&output->info;
	for (unsigned index = 0; index < sizeof(output->info); ++index)
		TEST_CHECK_EQ(0xa5, bytes[index]);
}

static struct CallResult call_info(const char *name, UINT code_page, DWORD flags, LPCPINFOEXW output) {
	SetLastError(0x4321);
	struct CallResult result;
	result.success = get_cp_info(code_page, flags, output);
	result.error = GetLastError();
	printf("%s: page=%u flags=%lu success=%d error=%lu\n", name, code_page, (unsigned long)flags, result.success,
		   (unsigned long)result.error);
	return result;
}

static void check_failure(struct CallResult result, DWORD error) {
	TEST_CHECK_EQ(FALSE, result.success);
	TEST_CHECK_EQ(error, result.error);
}

static void check_success(struct CallResult result) {
	TEST_CHECK(result.success != FALSE);
	TEST_CHECK_EQ(0x4321, result.error);
}

static void check_info(const struct GuardedInfo *output, UINT expected_page) {
	check_bounds(output);
	TEST_CHECK_EQ(expected_page, output->info.CodePage);
	TEST_CHECK(output->info.MaxCharSize > 0);
	unsigned name_units = 0;
	while (name_units < MAX_PATH && output->info.CodePageName[name_units])
		++name_units;
	TEST_CHECK(name_units > 0 && name_units < MAX_PATH);
}

static void print_data(const CPINFOEXW *info) {
	printf("resolved=%u size=%u default=%02x,%02x unicode=%04x lead:", info->CodePage, info->MaxCharSize,
		   info->DefaultChar[0], info->DefaultChar[1], info->UnicodeDefaultChar);
	for (unsigned index = 0; index < MAX_LEADBYTES; ++index)
		printf(" %02x", info->LeadByte[index]);
	putchar('\n');
}

static void check_no_leads(const CPINFOEXW *info) {
	for (unsigned index = 0; index < MAX_LEADBYTES; ++index)
		TEST_CHECK_EQ(0, info->LeadByte[index]);
}

static void check_defaults(const CPINFOEXW *info, WCHAR unicode_default) {
	TEST_CHECK_EQ('?', info->DefaultChar[0]);
	TEST_CHECK_EQ(0, info->DefaultChar[1]);
	TEST_CHECK_EQ(unicode_default, info->UnicodeDefaultChar);
}

static void test_explicit_pages(void) {
	struct GuardedInfo utf8, single_byte, double_byte, repeated;
	reset_info(&utf8);
	reset_info(&single_byte);
	reset_info(&double_byte);
	reset_info(&repeated);
	check_success(call_info("utf8", CP_UTF8, 0, &utf8.info));
	check_info(&utf8, CP_UTF8);
	TEST_CHECK_EQ(4, utf8.info.MaxCharSize);
	check_no_leads(&utf8.info);
	check_defaults(&utf8.info, 0xfffd);
	print_data(&utf8.info);
	check_success(call_info("single-byte", 1252, 0, &single_byte.info));
	check_info(&single_byte, 1252);
	TEST_CHECK_EQ(1, single_byte.info.MaxCharSize);
	check_no_leads(&single_byte.info);
	check_defaults(&single_byte.info, '?');
	print_data(&single_byte.info);
	check_success(call_info("double-byte", 932, 0, &double_byte.info));
	check_info(&double_byte, 932);
	TEST_CHECK_EQ(2, double_byte.info.MaxCharSize);
	const BYTE expected_leads[MAX_LEADBYTES] = {0x81, 0x9f, 0xe0, 0xfc};
	TEST_CHECK(memcmp(double_byte.info.LeadByte, expected_leads, sizeof(expected_leads)) == 0);
	check_defaults(&double_byte.info, 0x30fb);
	print_data(&double_byte.info);
	check_success(call_info("utf8-repeat", CP_UTF8, 0, &repeated.info));
	check_info(&repeated, CP_UTF8);
	TEST_CHECK(memcmp(&utf8.info, &repeated.info, sizeof(utf8.info)) == 0);
}

static void test_default_pages(void) {
	const UINT aliases[] = {CP_ACP, CP_OEMCP, CP_MACCP};
	for (unsigned index = 0; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
		struct GuardedInfo alias, explicit_page;
		reset_info(&alias);
		reset_info(&explicit_page);
		check_success(call_info("default-page", aliases[index], 0, &alias.info));
		TEST_CHECK(alias.info.CodePage > CP_THREAD_ACP);
		check_info(&alias, alias.info.CodePage);
		if (aliases[index] == CP_ACP)
			TEST_CHECK_EQ(GetACP(), alias.info.CodePage);
		check_success(call_info("resolved-default-page", alias.info.CodePage, 0, &explicit_page.info));
		check_info(&explicit_page, alias.info.CodePage);
		TEST_CHECK(memcmp(&alias.info, &explicit_page.info, sizeof(alias.info)) == 0);
	}
}

static void test_errors(void) {
	struct GuardedInfo output;
	reset_info(&output);
	check_failure(call_info("invalid-page", 0xffffffffu, 0, &output.info), ERROR_INVALID_PARAMETER);
	check_unchanged(&output);
	check_failure(call_info("null-output", CP_UTF8, 0, NULL), ERROR_INVALID_PARAMETER);
}

static void test_local_scope(void) {
	struct GuardedInfo output;
	reset_info(&output);
	check_failure(call_info("thread-page", CP_THREAD_ACP, 0, &output.info), ERROR_NOT_SUPPORTED);
	check_failure(call_info("reserved-flags", CP_UTF8, 1, &output.info), ERROR_INVALID_PARAMETER);
	check_failure(call_info("null-output", CP_UTF8, 0, NULL), ERROR_INVALID_PARAMETER);
	check_unchanged(&output);
}

static void test_transport(const char *mode) {
	struct GuardedInfo output;
	reset_info(&output);
	struct CallResult result = call_info("transport", CP_UTF8, 0, &output.info);
	if (strcmp(mode, "success") == 0) {
		check_success(result);
		check_info(&output, CP_UTF8);
		TEST_CHECK_EQ(4, output.info.MaxCharSize);
		check_no_leads(&output.info);
		check_defaults(&output.info, 0xfffd);
	} else {
		const DWORD error = strcmp(mode, "failed") == 0		   ? ERROR_INVALID_PARAMETER
							: strcmp(mode, "failed-zero") == 0 ? ERROR_SUCCESS
							: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
															   : ERROR_INVALID_DATA;
		check_failure(result, error);
		check_unchanged(&output);
	}
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "GetCPInfoExW");
	_Static_assert(sizeof(exported) == sizeof(get_cp_info), "Resolved function pointers have the same width");
	memcpy(&get_cp_info, &exported, sizeof(get_cp_info));
	TEST_CHECK(get_cp_info != NULL);
	const char *transport_mode = getenv("WIBO_FIXTURE_CP_INFO_RESPONSE");
	if (transport_mode) {
		test_transport(transport_mode);
	} else if (getenv("WIBO_EXPECT_CP_INFO_LOCAL")) {
		test_local_scope();
	} else {
		test_explicit_pages();
		test_default_pages();
		test_errors();
	}
	return 0;
}
