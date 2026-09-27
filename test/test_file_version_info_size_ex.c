#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <wchar.h>

typedef DWORD(WINAPI *GetVersionSizeFn)(DWORD, LPCWSTR, LPDWORD);

struct GuardedHandle {
	BYTE before[16];
	DWORD value;
	BYTE after[16];
};

struct CallResult {
	DWORD size;
	DWORD error;
};

_Static_assert(sizeof(DWORD) == 4, "Version size and handle outputs are four-byte integers");

static GetVersionSizeFn get_version_size;

static struct GuardedHandle handle_value(void) {
	struct GuardedHandle result;
	memset(&result, 0xa5, sizeof(result));
	result.value = 0x11223344;
	return result;
}

static void check_bounds(const struct GuardedHandle *output) {
	for (unsigned index = 0; index < sizeof(output->before); ++index) {
		TEST_CHECK_EQ(0xa5, output->before[index]);
		TEST_CHECK_EQ(0xa5, output->after[index]);
	}
}

static struct CallResult call_size(const char *name, DWORD flags, LPCWSTR filename, struct GuardedHandle *handle) {
	SetLastError(0x4321);
	struct CallResult result;
	result.size = get_version_size(flags, filename, handle ? &handle->value : NULL);
	result.error = GetLastError();
	if (handle)
		check_bounds(handle);
	printf("%s: flags=%lu size=%lu error=%lu handle=%lu\n", name, (unsigned long)flags, (unsigned long)result.size,
		   (unsigned long)result.error, (unsigned long)(handle ? handle->value : 0));
	return result;
}

static void check_failure(struct CallResult result, DWORD error) {
	TEST_CHECK_EQ(0, result.size);
	TEST_CHECK_EQ(error, result.error);
}

static void sibling_path(const WCHAR *executable, const WCHAR *name, WCHAR output[MAX_PATH]) {
	const size_t length = wcslen(executable);
	size_t prefix = length;
	while (prefix && executable[prefix - 1] != '\\' && executable[prefix - 1] != '/')
		--prefix;
	const size_t name_units = wcslen(name) + 1;
	TEST_CHECK(prefix + name_units <= MAX_PATH);
	memcpy(output, executable, prefix * sizeof(WCHAR));
	memcpy(output + prefix, name, name_units * sizeof(WCHAR));
}

static unsigned hex_digit(char value) {
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	TEST_CHECK(value >= 'A' && value <= 'F');
	return value - 'A' + 10;
}

static void decode_fixture_path(const char *encoded, WCHAR output[MAX_PATH]) {
	const size_t bytes = strlen(encoded);
	TEST_CHECK(bytes > 0 && bytes % 4 == 0 && bytes / 4 < MAX_PATH);
	for (size_t index = 0; index < bytes / 4; ++index) {
		const char *unit = encoded + index * 4;
		output[index] = (WCHAR)((hex_digit(unit[0]) << 4) | hex_digit(unit[1]) | (hex_digit(unit[2]) << 12) |
								(hex_digit(unit[3]) << 8));
		TEST_CHECK(output[index] != 0);
	}
	output[bytes / 4] = 0;
}

static void test_sizes(void) {
	WCHAR executable[MAX_PATH];
	DWORD path_units = GetModuleFileNameW(NULL, executable, MAX_PATH);
	TEST_CHECK(path_units && path_units < MAX_PATH);
	HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(RT_VERSION));
	TEST_CHECK(resource != NULL);
	DWORD raw_size = SizeofResource(NULL, resource);
	TEST_CHECK(raw_size > 0);
	printf("embedded-resource-size=%lu\n", (unsigned long)raw_size);
	const DWORD flags[] = {0, FILE_VER_GET_LOCALISED, FILE_VER_GET_NEUTRAL};
	struct CallResult baseline[sizeof(flags) / sizeof(flags[0])];
	for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
		struct GuardedHandle handle = handle_value();
		struct CallResult result = call_size("with-handle", flags[index], executable, &handle);
		TEST_CHECK(result.size >= raw_size);
		TEST_CHECK_EQ(0, handle.value);
		baseline[index] = result;
		struct CallResult optional = call_size("without-handle", flags[index], executable, NULL);
		TEST_CHECK_EQ(result.size, optional.size);
		TEST_CHECK_EQ(result.error, optional.error);
	}
	const char *fixture_path = getenv("WIBO_FIXTURE_VERSION_FILE_UTF16");
	if (fixture_path) {
		WCHAR filename[MAX_PATH];
		TEST_CHECK(*fixture_path);
		decode_fixture_path(fixture_path, filename);
		for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
			struct GuardedHandle handle = handle_value();
			struct CallResult result = call_size("fixture-file", flags[index], filename, &handle);
			TEST_CHECK_EQ(baseline[index].size, result.size);
			TEST_CHECK_EQ(baseline[index].error, result.error);
			TEST_CHECK_EQ(0, handle.value);
		}
	}

	WCHAR no_resource[MAX_PATH], missing[MAX_PATH];
	sibling_path(executable, L"file_version_no_resource.dll", no_resource);
	sibling_path(executable, L"synthetic_absent_version_file.dll", missing);
	struct GuardedHandle handle = handle_value();
	check_failure(call_size("no-resource", FILE_VER_GET_LOCALISED, no_resource, &handle),
				  ERROR_RESOURCE_DATA_NOT_FOUND);
	TEST_CHECK_EQ(0, handle.value);
	handle = handle_value();
	check_failure(call_size("missing-file", FILE_VER_GET_LOCALISED, missing, &handle), ERROR_FILE_NOT_FOUND);
	TEST_CHECK_EQ(0, handle.value);
	handle = handle_value();
	check_failure(call_size("null-filename", FILE_VER_GET_LOCALISED, NULL, &handle), ERROR_INVALID_PARAMETER);
	TEST_CHECK_EQ(0, handle.value);
	handle = handle_value();
	check_failure(call_size("empty-filename", FILE_VER_GET_LOCALISED, L"", &handle), ERROR_BAD_PATHNAME);
	TEST_CHECK_EQ(0, handle.value);
	if (getenv("WIBO_OBSERVE_VERSION_UNKNOWN_FLAGS")) {
		handle = handle_value();
		call_size("unknown-flags", 0x80000000, executable, &handle);
	}
}

static void test_transport(const char *mode) {
	struct GuardedHandle handle = handle_value();
	BOOL optional_handle = strcmp(mode, "no-handle") != 0 && strcmp(mode, "unexpected-handle") != 0;
	LPCWSTR filename = L"Z:\\Fixture\\version-size.exe";
	if (strcmp(mode, "null-filename") == 0)
		filename = NULL;
	else if (strcmp(mode, "empty-filename") == 0)
		filename = L"";
	struct CallResult result =
		call_size("transport", FILE_VER_GET_LOCALISED, filename, optional_handle ? &handle : NULL);
	DWORD expected_size = 0, expected_error = ERROR_INVALID_DATA, expected_handle = 0x11223344;
	if (strcmp(mode, "success") == 0 || strcmp(mode, "success-preserved") == 0 || strcmp(mode, "no-handle") == 0 ||
		strcmp(mode, "unwritten-handle") == 0) {
		expected_size = 732;
		expected_error = strcmp(mode, "success-preserved") == 0 ? 0x4321 : ERROR_SUCCESS;
		if (optional_handle && strcmp(mode, "unwritten-handle") != 0)
			expected_handle = 0;
	} else if (strcmp(mode, "zero-result") == 0 || strcmp(mode, "zero-error") == 0 ||
			   strcmp(mode, "null-filename") == 0 || strcmp(mode, "empty-filename") == 0) {
		expected_handle = 0;
		if (strcmp(mode, "zero-result") == 0)
			expected_error = ERROR_RESOURCE_DATA_NOT_FOUND;
		else if (strcmp(mode, "zero-error") == 0)
			expected_error = ERROR_SUCCESS;
		else if (strcmp(mode, "null-filename") == 0)
			expected_error = ERROR_INVALID_PARAMETER;
		else
			expected_error = ERROR_BAD_PATHNAME;
	} else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_ACCESS_DENIED;
	else if (strcmp(mode, "unavailable") == 0 || strcmp(mode, "namespace-unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	TEST_CHECK_EQ(expected_size, result.size);
	TEST_CHECK_EQ(expected_error, result.error);
	TEST_CHECK_EQ(expected_handle, handle.value);
	check_bounds(&handle);
}

static void test_local_scope(void) {
	const WCHAR *filenames[] = {L"relative-version.dll", L"Q:\\Fixture\\version-size.exe",
								L"\\\\fixture-host\\share\\version-size.exe", L"\\root\\version-size.exe"};
	for (unsigned index = 0; index < sizeof(filenames) / sizeof(filenames[0]); ++index) {
		struct GuardedHandle handle = handle_value();
		check_failure(call_size("unsupported-path", FILE_VER_GET_LOCALISED, filenames[index], &handle),
					  ERROR_NOT_SUPPORTED);
		TEST_CHECK_EQ(0x11223344, handle.value);
	}
}

int main(void) {
	HMODULE module = LoadLibraryW(L"version.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "GetFileVersionInfoSizeExW");
	_Static_assert(sizeof(exported) == sizeof(get_version_size), "Resolved function pointers have the same width");
	memcpy(&get_version_size, &exported, sizeof(get_version_size));
	TEST_CHECK(get_version_size != NULL);
	const char *mode = getenv("WIBO_FIXTURE_VERSION_SIZE_RESPONSE");
	if (mode)
		test_transport(mode);
	else if (getenv("WIBO_EXPECT_VERSION_SIZE_LOCAL"))
		test_local_scope();
	else
		test_sizes();
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
