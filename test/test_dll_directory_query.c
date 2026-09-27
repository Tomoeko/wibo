#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct GuardedBuffer {
	uint64_t before;
	char bytes[256];
	uint64_t after;
};

static const uint64_t guard = UINT64_C(0x196dc58a320f7eb4);

static int query(const char *expected, DWORD capacity, int present) {
	struct GuardedBuffer output;
	memset(&output, 0xa5, sizeof(output));
	output.before = output.after = guard;
	SetLastError(0x4321);
	DWORD result = GetDllDirectoryA(capacity, present ? output.bytes : NULL);
	DWORD error = GetLastError();
	size_t length = strlen(expected);
	DWORD required = (DWORD)length + 1;
	printf("query expected-length=%lu capacity=%lu present=%d result=%lu error=%lu first=%u\n", (unsigned long)length,
		   (unsigned long)capacity, present, (unsigned long)result, (unsigned long)error,
		   (unsigned)(unsigned char)output.bytes[0]);
	if (output.before != guard || output.after != guard || result != (present && capacity > length ? length : required))
		return 0;
	if (present && capacity > length && memcmp(output.bytes, expected, length + 1))
		return 0;
	if (present && capacity > 0 && capacity <= length && output.bytes[0] != 0)
		return 0;
	size_t untouchedStart = present ? (capacity ? capacity : 1) : 0;
	for (size_t index = untouchedStart; index < sizeof(output.bytes); ++index)
		if ((unsigned char)output.bytes[index] != 0xa5)
			return 0;
	// Zero-capacity output and successful last-error values are observed separately.
	return 1;
}

static int checkSetting(const char *setting, const char *expected) {
	SetLastError(0x4321);
	BOOL result = SetDllDirectoryA(setting);
	DWORD error = GetLastError();
	printf("set value=%s result=%ld error=%lu\n", setting ? setting : "<null>", (long)result, (unsigned long)error);
	if (!result)
		return 0;
	size_t length = strlen(expected);
	if (!query(expected, 0, 0) || !query(expected, 0, 1) || !query(expected, 1, 1) ||
		!query(expected, (DWORD)length, 1) || !query(expected, (DWORD)length + 1, 1) ||
		!query(expected, sizeof(((struct GuardedBuffer *)0)->bytes), 1))
		return 0;
	return 1;
}

int main(void) {
	int result = 1;
	char original[32768];
	DWORD length = GetDllDirectoryA(sizeof(original), original);
	if (length >= sizeof(original))
		return 1;
	original[length] = 0;
	if (!checkSetting(NULL, "") || !checkSetting("", "") || !checkSetting(".", ".") ||
		!checkSetting("synthetic-directory\\.\\relative\\..", "synthetic-directory\\.\\relative\\..") ||
		!checkSetting("synthetic-directory/relative/", "synthetic-directory/relative/"))
		goto cleanup;
	SetLastError(0x4321);
	BOOL wideResult = SetDllDirectoryW(L"relative-wide\\.");
	printf("set-wide result=%ld error=%lu\n", (long)wideResult, (unsigned long)GetLastError());
	if (!wideResult || !query("relative-wide\\.", 0, 0) || !query("relative-wide\\.", 256, 1))
		goto cleanup;
	if (!checkSetting(NULL, ""))
		goto cleanup;
	result = 0;
cleanup:
	if (!SetDllDirectoryA(original[0] ? original : NULL))
		result = 1;
	if (result)
		fprintf(stderr, "DLL directory query fixture failed: error=%lu\n", (unsigned long)GetLastError());
	return result;
}
