#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef struct {
	USHORT Length;
	USHORT MaximumLength;
	void *Buffer;
} CountedString;

typedef void(WINAPI *InitFn)(CountedString *, const void *);
typedef LONG(WINAPI *InitExFn)(CountedString *, const void *);

typedef struct {
	uint64_t before;
	CountedString value;
	uint64_t after;
} GuardedString;

static const DWORD kLastError = 0x13579bdfu;
static const LONG kNameTooLong = (LONG)0xc0000106u;

static FARPROC load_initializer(const char *name) {
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(module != NULL);
	FARPROC proc = GetProcAddress(module, name);
	TEST_CHECK(proc != NULL);
	return proc;
}

static void check_initializer(const char *name, int is_ex, void *source, size_t units, size_t width,
							  USHORT expected_length, USHORT expected_maximum, LONG expected_status) {
	FARPROC proc = load_initializer(name);
	GuardedString output;
	memset(&output, 0xa5, sizeof(output));
	GuardedString original;
	memcpy(&original, &output, sizeof(original));
	size_t source_bytes = source ? (units + 1) * width : 0;
	unsigned char *source_copy = NULL;
	if (source_bytes) {
		source_copy = malloc(source_bytes);
		TEST_CHECK(source_copy != NULL);
		memcpy(source_copy, source, source_bytes);
	}
	SetLastError(kLastError);
	LONG status = 0;
	if (is_ex) {
		InitExFn fn;
		TEST_CHECK_EQ(sizeof(fn), sizeof(proc));
		memcpy(&fn, &proc, sizeof(fn));
		status = fn(&output.value, source);
	} else {
		InitFn fn;
		TEST_CHECK_EQ(sizeof(fn), sizeof(proc));
		memcpy(&fn, &proc, sizeof(fn));
		fn(&output.value, source);
	}
	DWORD error = GetLastError();
	TEST_CHECK_EQ(expected_status, status);
	TEST_CHECK_EQ(kLastError, error);
	TEST_CHECK_U64_EQ(original.before, output.before);
	TEST_CHECK_U64_EQ(original.after, output.after);
	if (status) {
		TEST_CHECK_EQ(0, memcmp(&original.value, &output.value, sizeof(output.value)));
	} else {
		TEST_CHECK_EQ(expected_length, output.value.Length);
		TEST_CHECK_EQ(expected_maximum, output.value.MaximumLength);
		TEST_CHECK(output.value.Buffer == source);
		for (size_t i = 4; i < offsetof(CountedString, Buffer); ++i)
			TEST_CHECK_EQ(0xa5, ((const unsigned char *)&output.value)[i]);
		if (units) {
			unsigned char previous = ((unsigned char *)source)[0];
			((unsigned char *)source)[0] ^= 1;
			TEST_CHECK_EQ(previous ^ 1, ((const unsigned char *)output.value.Buffer)[0]);
			((unsigned char *)source)[0] = previous;
		}
	}
	if (source_bytes)
		TEST_CHECK_EQ(0, memcmp(source_copy, source, source_bytes));
	free(source_copy);
}

static void test_ansi_initializers(int documented_boundaries) {
	const char *names[] = {"RtlInitString", "RtlInitAnsiString", "RtlInitAnsiStringEx"};
	const size_t lengths[] = {0, 1, 3, 65533, 65534, 65535, 65536, 65537, 131071};
	char *source = malloc(131072);
	TEST_CHECK(source != NULL);
	for (size_t api = 0; api < sizeof(names) / sizeof(*names); ++api) {
		int is_ex = api == 2;
		if (!documented_boundaries) {
			check_initializer(names[api], is_ex, NULL, 0, 1, 0, 0, 0);
			unsigned char raw[] = {0x80, 0xff, 0, 'q', 0};
			check_initializer(names[api], is_ex, raw, 2, 1, 2, 3, 0);
		}
		for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) {
			size_t length = lengths[i];
			if (documented_boundaries ? (is_ex || length <= 65534) : (!is_ex && length > 65534))
				continue;
			memset(source, 'q', length);
			source[length] = 0;
			LONG status = is_ex && length > 65534 ? kNameTooLong : 0;
			size_t counted_length = length > 65534 ? 65534 : length;
			check_initializer(names[api], is_ex, source, length, 1, (USHORT)counted_length,
							  (USHORT)(counted_length + 1), status);
		}
	}
	free(source);
}

static void test_unicode_initializers(void) {
	const char *names[] = {"RtlInitUnicodeString", "RtlInitUnicodeStringEx"};
	const size_t lengths[] = {0, 1, 3, 32765, 32766, 32767, 32768, 65535};
	WCHAR *source = malloc(65536 * sizeof(WCHAR));
	TEST_CHECK(source != NULL);
	for (size_t api = 0; api < sizeof(names) / sizeof(*names); ++api) {
		int is_ex = api == 1;
		check_initializer(names[api], is_ex, NULL, 0, 2, 0, 0, 0);
		WCHAR raw[] = {0xd800, 0xdc00, 0xdc00, 0, 0x1234, 0};
		check_initializer(names[api], is_ex, raw, 3, 2, 6, 8, 0);
		for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i) {
			size_t length = lengths[i];
			for (size_t j = 0; j < length; ++j)
				source[j] = 0x1234;
			source[length] = 0;
			size_t bytes = length > 32766 ? 65532 : length * sizeof(WCHAR);
			LONG status = is_ex && length > 32766 ? kNameTooLong : 0;
			check_initializer(names[api], is_ex, source, length, 2, (USHORT)bytes, (USHORT)(bytes + 2), status);
		}
	}
	free(source);
}

int main(int argc, char **argv) {
	TEST_CHECK_EQ(sizeof(void *) == 8 ? 16 : 8, sizeof(CountedString));
	TEST_CHECK_EQ(sizeof(void *) == 8 ? 8 : 4, offsetof(CountedString, Buffer));
	int documented_boundaries = argc == 2 && strcmp(argv[1], "--documented-boundaries") == 0;
	TEST_CHECK(argc == 1 || documented_boundaries);
	test_ansi_initializers(documented_boundaries);
	if (!documented_boundaries)
		test_unicode_initializers();
	return 0;
}
