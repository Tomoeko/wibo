#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

#define SEED 0x4321
#define CAPACITY 512
#define GUARD UINT64_C(0x5a93e81fc247b60d)

static unsigned calls;

static void check(int wide, const void *source, DWORD extra, DWORD capacity, va_list *arguments, const void *expected,
				  DWORD length, DWORD error) {
	const size_t unit = wide ? sizeof(WCHAR) : 1;
	const DWORD flags = FORMAT_MESSAGE_FROM_STRING | extra;
	DWORD result, observed;
	++calls;
	if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
		struct {
			uint64_t before;
			void *pointer;
			uint64_t after;
		} allocation = {GUARD, NULL, GUARD};
		SetLastError(SEED);
		result = wide ? FormatMessageW(flags, source, 0x1234, 0x409, (LPWSTR)&allocation.pointer, capacity, arguments)
					  : FormatMessageA(flags, source, 0x1234, 0x409, (LPSTR)&allocation.pointer, capacity, arguments);
		observed = GetLastError();
		TEST_CHECK_U64_EQ(GUARD, allocation.before);
		TEST_CHECK_U64_EQ(GUARD, allocation.after);
		TEST_CHECK_EQ(length, result);
		TEST_CHECK_EQ(error, observed);
		if (result) {
			TEST_CHECK(allocation.pointer != NULL);
			TEST_CHECK(LocalSize(allocation.pointer) >= (size_t)(result + 1) * unit);
			TEST_CHECK(LocalSize(allocation.pointer) >= (size_t)capacity * unit);
			TEST_CHECK(memcmp(allocation.pointer, expected, (size_t)(result + 1) * unit) == 0);
			TEST_CHECK(LocalFree(allocation.pointer) == NULL);
		} else
			TEST_CHECK(allocation.pointer == NULL);
	} else {
		struct {
			uint64_t before;
			WCHAR buffer[CAPACITY + 1];
			uint64_t after;
		} storage;
		BYTE expectedBuffer[sizeof(storage.buffer)];
		memset(&storage, 0xa5, sizeof(storage));
		memset(expectedBuffer, 0xa5, sizeof(expectedBuffer));
		storage.before = storage.after = GUARD;
		TEST_CHECK(capacity <= CAPACITY);
		if (length)
			memcpy(expectedBuffer, expected, (size_t)(length + 1) * unit);
		else if (wide && capacity && error == ERROR_INSUFFICIENT_BUFFER) {
			const size_t copied = capacity - 1;
			memcpy(expectedBuffer, source, copied * unit);
			memset(expectedBuffer + copied * unit, 0, unit);
		} else if (wide && capacity && error == ERROR_NO_WORK_DONE)
			memset(expectedBuffer, 0, unit);
		SetLastError(SEED);
		result = wide ? FormatMessageW(flags, source, 0, 0, storage.buffer, capacity, arguments)
					  : FormatMessageA(flags, source, 0, 0, (LPSTR)storage.buffer, capacity, arguments);
		observed = GetLastError();
		TEST_CHECK_U64_EQ(GUARD, storage.before);
		TEST_CHECK_U64_EQ(GUARD, storage.after);
		TEST_CHECK_EQ(length, result);
		TEST_CHECK_EQ(error, observed);
		TEST_CHECK(memcmp(storage.buffer, expectedBuffer, sizeof(expectedBuffer)) == 0);
	}
}

static void pair(const char *sourceA, const WCHAR *sourceW, DWORD flags, DWORD capacity, const char *expectedA,
				 const WCHAR *expectedW, DWORD length, DWORD error) {
	check(0, sourceA, flags, capacity, NULL, expectedA, length, error);
	check(1, sourceW, flags, capacity, NULL, expectedW, length, error);
}

static void variableA(const char *source, const char *expected, ...) {
	va_list arguments;
	va_start(arguments, expected);
	check(0, source, FORMAT_MESSAGE_ALLOCATE_BUFFER, 0, &arguments, expected, (DWORD)strlen(expected), SEED);
	va_end(arguments);
}

static void variableW(const WCHAR *source, const WCHAR *expected, ...) {
	va_list arguments;
	va_start(arguments, expected);
	check(1, source, FORMAT_MESSAGE_ALLOCATE_BUFFER, 0, &arguments, expected, (DWORD)wcslen(expected), SEED);
	va_end(arguments);
}

static void fault(const char *mode) {
	struct {
		uint64_t before;
		WCHAR buffer[32];
		uint64_t after;
	} storage;
	memset(&storage, 0xa5, sizeof(storage));
	storage.before = storage.after = GUARD;
	BYTE original[sizeof(storage.buffer)];
	memcpy(original, storage.buffer, sizeof(original));
	DWORD_PTR arguments[] = {(DWORD_PTR)L"first", 37, (DWORD_PTR)L"third"};
	if (strncmp(mode, "allocated-", 10) == 0) {
		WCHAR *allocated = NULL;
		SetLastError(SEED);
		TEST_CHECK_EQ(0, FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY |
											FORMAT_MESSAGE_ALLOCATE_BUFFER,
										L"%1/%2!u!/%3/%1", 0, 0, (LPWSTR)&allocated, 0, (va_list *)arguments));
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(allocated == NULL);
		return;
	}
	SetLastError(SEED);
	TEST_CHECK_EQ(0, FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, L"%1/%2!u!/%3/%1", 0, 0,
									storage.buffer, 32, (va_list *)arguments));
	const DWORD expected = (strcmp(mode, "unavailable") == 0 || strcmp(mode, "unconfigured") == 0) ? ERROR_NOT_SUPPORTED
						   : strcmp(mode, "failed") == 0										   ? ERROR_ACCESS_DENIED
																								   : ERROR_INVALID_DATA;
	TEST_CHECK_EQ(expected, GetLastError());
	TEST_CHECK_U64_EQ(GUARD, storage.before);
	TEST_CHECK_U64_EQ(GUARD, storage.after);
	TEST_CHECK(memcmp(original, storage.buffer, sizeof(original)) == 0);
	if (strcmp(mode, "retry") == 0) {
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_MESSAGE_STRING_RESPONSE", "success"));
		check(1, L"%1/%2!u!/%3/%1", FORMAT_MESSAGE_ARGUMENT_ARRAY, 32, (va_list *)arguments, L"first/37/third/first",
			  20, SEED);
	}
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "local-limits") == 0) {
		WCHAR output[16];
		memset(output, 0xa5, sizeof(output));
		WCHAR original[16];
		memcpy(original, output, sizeof(output));
		TEST_CHECK_EQ(0, FormatMessageW(FORMAT_MESSAGE_FROM_STRING, L"literal", 0, 0, output, UINT32_MAX, NULL));
		TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, GetLastError());
		TEST_CHECK(memcmp(original, output, sizeof(output)) == 0);
		DWORD_PTR argument = 1;
		TEST_CHECK_EQ(0, FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, L"%1!I64u!", 0, 0,
										output, 16, (va_list *)&argument));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(memcmp(original, output, sizeof(output)) == 0);
		TEST_CHECK_EQ(0, FormatMessageW(FORMAT_MESSAGE_FROM_STRING, L"%1!f!", 0, 0, output, 16, NULL));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(memcmp(original, output, sizeof(output)) == 0);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "all") != 0) {
		fault(argv[1]);
		return 0;
	}
	const DWORD allocated = FORMAT_MESSAGE_ALLOCATE_BUFFER;
	pair("literal message", L"literal message", allocated, 0, "literal message", L"literal message", 15, SEED);
	pair("literal message", L"literal message", allocated, 128, "literal message", L"literal message", 15, SEED);
	pair("literal message", L"literal message", 0, CAPACITY, "literal message", L"literal message", 15, SEED);
	pair("abc", L"abc", 0, 4, "abc", L"abc", 3, SEED);
	for (DWORD size = 0; size <= 3; ++size)
		pair("abc", L"abc", 0, size, NULL, NULL, 0, ERROR_INSUFFICIENT_BUFFER);
	pair("a%%b%.%!% c%tD%rE%nF%0ignored", L"a%%b%.%!% c%tD%rE%nF%0ignored", allocated, 0, "a%b.! c\tD\rE\r\nF",
		 L"a%b.! c\tD\rE\r\nF", 14, SEED);
	pair("a%qz", L"a%qz", allocated, 0, "aqz", L"aqz", 3, SEED);
	pair("%1 %2!08X! %% %n end", L"%1 %2!08X! %% %n end", allocated | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
		 "%1 %2!08X! %% \r\n end", L"%1 %2!08X! %% \r\n end", 20, SEED);
	pair("one\ntwo\rthree\r\nfour%nend", L"one\ntwo\rthree\r\nfour%nend", allocated, 0,
		 "one\r\ntwo\r\nthree\r\nfour\r\nend", L"one\r\ntwo\r\nthree\r\nfour\r\nend", 26, SEED);
	pair("one\ntwo\rthree\r\nfour%nend", L"one\ntwo\rthree\r\nfour%nend", allocated | 255, 0,
		 "one two three four\r\nend", L"one two three four\r\nend", 23, SEED);
	pair("one two three four%nlongword", L"one two three four%nlongword", allocated | 8, 0,
		 "one two\r\nthree\r\nfour\r\nlongword\r\n", L"one two\r\nthree\r\nfour\r\nlongword\r\n", 32, SEED);
	pair("", L"", allocated, 0, NULL, NULL, 0, ERROR_NO_WORK_DONE);
	pair("", L"", 0, CAPACITY, NULL, NULL, 0, ERROR_NO_WORK_DONE);
	pair("prefix%", L"prefix%", allocated, 0, NULL, NULL, 0, ERROR_INVALID_PARAMETER);
	pair("literal", L"literal", allocated | FORMAT_MESSAGE_FROM_SYSTEM, 0, "literal", L"literal", 7, SEED);
	DWORD_PTR argumentsA[] = {(DWORD_PTR) "first", 37, (DWORD_PTR) "third"};
	DWORD_PTR argumentsW[] = {(DWORD_PTR)L"first", 37, (DWORD_PTR)L"third"};
	check(0, "%1/%2!u!/%3/%1", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)argumentsA,
		  "first/37/third/first", 20, SEED);
	check(1, L"%1/%2!u!/%3/%1", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)argumentsW,
		  L"first/37/third/first", 20, SEED);
	DWORD_PTR numbers[] = {(DWORD_PTR)(LONG_PTR)-7, 42, 0x12af};
	check(0, "%1!d!/%2!05u!/%3!08X!", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)numbers,
		  "-7/00042/000012AF", 17, SEED);
	check(1, L"%1!d!/%2!05u!/%3!08X!", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)numbers,
		  L"-7/00042/000012AF", 17, SEED);
	DWORD_PTR starsA[] = {4, 2, (DWORD_PTR) "Bill", (DWORD_PTR) "Bob", 6, (DWORD_PTR) "Bill"};
	DWORD_PTR starsW[] = {4, 2, (DWORD_PTR)L"Bill", (DWORD_PTR)L"Bob", 6, (DWORD_PTR)L"Bill"};
	check(0, "%1!*.*s! %4 %5!*s!", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)starsA, "  Bi Bob   Bill",
		  15, SEED);
	check(1, L"%1!*.*s! %4 %5!*s!", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)starsW, L"  Bi Bob   Bill",
		  15, SEED);
	const char *cursorSourcesA[] = {"%3!u! %1!*s!", "%1!*s! %1!*s!", "%1!*.*s! %1!*s!", "%3!u! %1!*.*s!"};
	const WCHAR *cursorSourcesW[] = {L"%3!u! %1!*s!", L"%1!*s! %1!*s!", L"%1!*.*s! %1!*s!", L"%3!u! %1!*.*s!"};
	const char *cursorResultsA[] = {"37 Next", "Bill Next", "  Bi Next", "37   Ne"};
	const WCHAR *cursorResultsW[] = {L"37 Next", L"Bill Next", L"  Bi Next", L"37   Ne"};
	for (unsigned wide = 0; wide != 2; ++wide) {
		for (unsigned mode = 0; mode != 4; ++mode) {
			const DWORD_PTR bill = wide ? (DWORD_PTR)L"Bill" : (DWORD_PTR) "Bill";
			const DWORD_PTR next = wide ? (DWORD_PTR)L"Next" : (DWORD_PTR) "Next";
			DWORD_PTR fields[] = {4, bill, 37, next, 0};
			if (mode == 1)
				fields[2] = next;
			if (mode == 2) {
				fields[1] = 2;
				fields[2] = bill;
			}
			if (mode == 3) {
				fields[3] = 2;
				fields[4] = next;
			}
			check(wide, wide ? (const void *)cursorSourcesW[mode] : cursorSourcesA[mode], FORMAT_MESSAGE_ARGUMENT_ARRAY,
				  128, (va_list *)fields, wide ? (const void *)cursorResultsW[mode] : cursorResultsA[mode],
				  (DWORD)strlen(cursorResultsA[mode]), SEED);
		}
	}
	DWORD_PTR highA[99] = {0}, highW[99] = {0};
	highA[98] = (DWORD_PTR) "slot";
	highW[98] = (DWORD_PTR)L"slot";
	check(0, "%99", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)highA, "slot", 4, SEED);
	check(1, L"%99", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, (va_list *)highW, L"slot", 4, SEED);
	pair("%1!", L"%1!", allocated | FORMAT_MESSAGE_ARGUMENT_ARRAY, 0, NULL, NULL, 0, ERROR_INVALID_PARAMETER);
	variableA("%1/%2!d!/%1", "first/-7/first", "first", -7);
	variableW(L"%1/%2!d!/%1", L"first/-7/first", L"first", -7);
	variableA("%1!*.*s! %3 %4!*s!", "  Bi Bob   Bill", 4, 2, "Bill", "Bob", 6, "Bill");
	variableW(L"%1!*.*s! %3 %4!*s!", L"  Bi Bob   Bill", 4, 2, L"Bill", L"Bob", 6, L"Bill");
	variableA("%1!I64u!", "4294967298", UINT64_C(4294967298));
	variableW(L"%1!I64u!", L"4294967298", UINT64_C(4294967298));
	const char ansi[] = {(char)0x80, (char)0xe9, 0};
	const WCHAR unicode[] = {0x20ac, 0x00e9, 0xd83d, 0xde00, 0};
	check(0, ansi, allocated, 0, NULL, ansi, 2, SEED);
	check(1, unicode, allocated, 0, NULL, unicode, 4, SEED);
	printf("FormatMessage source cases=%u\n", calls);
	return 0;
}
