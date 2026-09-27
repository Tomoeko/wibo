#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include "test_assert.h"
#include <windows.h>

#include <wchar.h>

typedef DWORD(WINAPI *VersionSizeFn)(DWORD, LPCWSTR, LPDWORD);
typedef BOOL(WINAPI *VersionInfoFn)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);

enum { BUFFER_CAPACITY = 4096 };

struct GuardedBuffer {
	BYTE before[16];
	BYTE bytes[BUFFER_CAPACITY];
	BYTE after[16];
};

struct CallResult {
	BOOL result;
	DWORD error;
};

static VersionSizeFn get_version_size;
static VersionInfoFn get_version_info;

static void initialize_buffer(struct GuardedBuffer *buffer) { memset(buffer, 0xa5, sizeof(*buffer)); }

static void check_bounds(const struct GuardedBuffer *buffer, DWORD capacity) {
	TEST_CHECK(capacity <= BUFFER_CAPACITY);
	for (unsigned index = 0; index < sizeof(buffer->before); ++index) {
		TEST_CHECK_EQ(0xa5, buffer->before[index]);
		TEST_CHECK_EQ(0xa5, buffer->after[index]);
	}
	for (DWORD index = capacity; index < BUFFER_CAPACITY; ++index)
		TEST_CHECK_EQ(0xa5, buffer->bytes[index]);
}

static struct CallResult call_info(const char *name, DWORD flags, LPCWSTR filename, DWORD handle, DWORD capacity,
								   struct GuardedBuffer *output) {
	SetLastError(0x4321);
	struct CallResult result;
	result.result = get_version_info(flags, filename, handle, capacity, output ? output->bytes : NULL);
	result.error = GetLastError();
	if (output)
		check_bounds(output, capacity);
	printf("%s: flags=%lu handle=%lu capacity=%lu result=%ld error=%lu\n", name, (unsigned long)flags,
		   (unsigned long)handle, (unsigned long)capacity, (long)result.result, (unsigned long)result.error);
	return result;
}

static void sibling_path(const WCHAR *executable, const WCHAR *name, WCHAR output[MAX_PATH]) {
	size_t prefix = wcslen(executable);
	while (prefix && executable[prefix - 1] != '\\' && executable[prefix - 1] != '/')
		--prefix;
	const size_t name_units = wcslen(name) + 1;
	TEST_CHECK(prefix + name_units <= MAX_PATH);
	memcpy(output, executable, prefix * sizeof(WCHAR));
	memcpy(output + prefix, name, name_units * sizeof(WCHAR));
}

static void test_native(void) {
	WCHAR executable[MAX_PATH];
	DWORD path_units = GetModuleFileNameW(NULL, executable, MAX_PATH);
	TEST_CHECK(path_units && path_units < MAX_PATH);
	HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(RT_VERSION));
	TEST_CHECK(resource != NULL);
	DWORD raw_size = SizeofResource(NULL, resource);
	TEST_CHECK(raw_size > 6 && raw_size < BUFFER_CAPACITY);
	HGLOBAL loaded = LoadResource(NULL, resource);
	TEST_CHECK(loaded != NULL);
	const BYTE *raw = LockResource(loaded);
	TEST_CHECK(raw != NULL);
	printf("embedded-resource-size=%lu\n", (unsigned long)raw_size);
	const DWORD flags[] = {0, FILE_VER_GET_LOCALISED, FILE_VER_GET_NEUTRAL,
						   FILE_VER_GET_LOCALISED | FILE_VER_GET_NEUTRAL,
						   FILE_VER_GET_LOCALISED | FILE_VER_GET_PREFETCHED};
	DWORD localized_size = 0;
	for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
		DWORD size = get_version_size(flags[index], executable, NULL);
		TEST_CHECK(size >= raw_size && size <= BUFFER_CAPACITY - 16);
		if (flags[index] == FILE_VER_GET_LOCALISED)
			localized_size = size;
		struct GuardedBuffer output, repeated;
		initialize_buffer(&output);
		struct CallResult result = call_info("exact", flags[index], executable, 0, size, &output);
		TEST_CHECK(result.result);
		TEST_CHECK_EQ(0, memcmp(raw, output.bytes, raw_size));
		initialize_buffer(&repeated);
		struct CallResult ignored = call_info("ignored-handle", flags[index], executable, 0x12345678, size, &repeated);
		TEST_CHECK(ignored.result);
		TEST_CHECK_EQ(result.error, ignored.error);
		TEST_CHECK_EQ(0, memcmp(output.bytes, repeated.bytes, size));
	}
	struct GuardedBuffer output;
	initialize_buffer(&output);
	struct CallResult generous =
		call_info("generous", FILE_VER_GET_LOCALISED, executable, 0, localized_size + 16, &output);
	TEST_CHECK(generous.result);
	TEST_CHECK_EQ(0, memcmp(raw, output.bytes, raw_size));
	initialize_buffer(&output);
	struct CallResult short_buffer = call_info("short", FILE_VER_GET_LOCALISED, executable, 0, raw_size - 1, &output);
	if (short_buffer.result)
		TEST_CHECK_EQ(0, memcmp(raw, output.bytes, raw_size - 1));
	struct CallResult null_buffer =
		call_info("null-output", FILE_VER_GET_LOCALISED, executable, 0, localized_size, NULL);
	TEST_CHECK(!null_buffer.result);

	WCHAR no_resource[MAX_PATH], missing[MAX_PATH];
	sibling_path(executable, L"file_version_no_resource.dll", no_resource);
	sibling_path(executable, L"synthetic_absent_version_file.dll", missing);
	const WCHAR *failures[] = {no_resource, missing};
	for (unsigned index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
		initialize_buffer(&output);
		struct CallResult result =
			call_info("absent-resource", FILE_VER_GET_LOCALISED, failures[index], 0, localized_size, &output);
		TEST_CHECK(!result.result);
		for (DWORD unit = 0; unit < localized_size; ++unit)
			TEST_CHECK_EQ(0xa5, output.bytes[unit]);
	}
}

static void test_transport(const char *mode) {
	struct GuardedBuffer output;
	initialize_buffer(&output);
	const BOOL null_data = strcmp(mode, "null-data") == 0 || strcmp(mode, "null-data-success") == 0 ||
						   strcmp(mode, "null-data-with-blob") == 0;
	struct CallResult result = call_info("transport", FILE_VER_GET_LOCALISED | FILE_VER_GET_NEUTRAL,
										 L"Z:\\Fixture\\version-info.exe", 0x12345678, 96, null_data ? NULL : &output);
	BOOL expected_result = FALSE, changed = FALSE;
	DWORD expected_error = ERROR_INVALID_DATA;
	if (strcmp(mode, "success") == 0 || strcmp(mode, "success-preserved") == 0) {
		expected_result = TRUE;
		changed = TRUE;
		expected_error = strcmp(mode, "success-preserved") == 0 ? 0x4321 : ERROR_SUCCESS;
	} else if (strcmp(mode, "false-mutated") == 0) {
		changed = TRUE;
		expected_error = ERROR_RESOURCE_DATA_NOT_FOUND;
	} else if (strcmp(mode, "false-zero") == 0)
		expected_error = ERROR_SUCCESS;
	else if (strcmp(mode, "failed") == 0)
		expected_error = ERROR_ACCESS_DENIED;
	else if (strcmp(mode, "unavailable") == 0)
		expected_error = ERROR_NOT_SUPPORTED;
	TEST_CHECK_EQ(expected_result, result.result);
	TEST_CHECK_EQ(expected_error, result.error);
	static const BYTE prefix[] = {0, 0x11, 0x22, 0x33, 0x80, 0xff, 0x42, 0};
	for (unsigned index = 0; index < BUFFER_CAPACITY; ++index) {
		BYTE expected = changed && index < sizeof(prefix) ? prefix[index] : 0xa5;
		TEST_CHECK_EQ(expected, output.bytes[index]);
	}
	check_bounds(&output, 96);
}

static void test_local_scope(void) {
	const DWORD unsupported_sizes[] = {0, 1, 91};
	for (unsigned index = 0; index < sizeof(unsupported_sizes) / sizeof(unsupported_sizes[0]); ++index) {
		struct GuardedBuffer output;
		initialize_buffer(&output);
		struct CallResult result = call_info("unsupported-size", FILE_VER_GET_LOCALISED,
											 L"Z:\\Fixture\\version-info.exe", 0, unsupported_sizes[index], &output);
		TEST_CHECK(!result.result);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, result.error);
		for (unsigned unit = 0; unit < BUFFER_CAPACITY; ++unit)
			TEST_CHECK_EQ(0xa5, output.bytes[unit]);
	}
	const DWORD large_capacity = 32 * 1024;
	BYTE *storage = malloc(large_capacity + 32);
	TEST_CHECK(storage != NULL);
	memset(storage, 0xa5, large_capacity + 32);
	SetLastError(0x4321);
	TEST_CHECK(
		!get_version_info(FILE_VER_GET_LOCALISED, L"Z:\\Fixture\\version-info.exe", 0, large_capacity, storage + 16));
	TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, GetLastError());
	for (DWORD index = 0; index < large_capacity + 32; ++index)
		TEST_CHECK_EQ(0xa5, storage[index]);
	free(storage);
}

int main(void) {
	HMODULE module = LoadLibraryW(L"version.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "GetFileVersionInfoSizeExW");
	_Static_assert(sizeof(exported) == sizeof(get_version_size), "Resolved function pointers have the same width");
	memcpy(&get_version_size, &exported, sizeof(get_version_size));
	TEST_CHECK(get_version_size != NULL);
	exported = GetProcAddress(module, "GetFileVersionInfoExW");
	_Static_assert(sizeof(exported) == sizeof(get_version_info), "Resolved function pointers have the same width");
	memcpy(&get_version_info, &exported, sizeof(get_version_info));
	TEST_CHECK(get_version_info != NULL);
	const char *mode = getenv("WIBO_FIXTURE_VERSION_INFO_RESPONSE");
	if (mode)
		test_transport(mode);
	else if (getenv("WIBO_EXPECT_VERSION_INFO_LOCAL"))
		test_local_scope();
	else
		test_native();
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
