// clang-format off: MinGW's dbghelp.h requires the Windows base types first.
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <string.h>

#include "test_assert.h"

static const DWORD seed = 0x13579bdf;

static void check_cleanup(HANDLE session, BOOL expected) {
	SetLastError(seed);
	TEST_CHECK_EQ(expected, !!SymCleanup(session));
	TEST_CHECK_EQ(seed, GetLastError());
}

static void check_base(HANDLE session, DWORD64 address, DWORD64 expected, DWORD error) {
	SetLastError(seed);
	TEST_CHECK_U64_EQ(expected, SymGetModuleBase64(session, address));
	TEST_CHECK_EQ(error, GetLastError());
}

static void check_unsupported_queries(HANDLE session, DWORD64 address, DWORD error) {
	DWORD64 displacement = seed;
	union {
		SYMBOL_INFO information;
		unsigned char bytes[sizeof(SYMBOL_INFO) + 32];
	} symbol;
	memset(&symbol, 0xa5, sizeof(symbol));
	symbol.information.SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol.information.MaxNameLen = 32;
	unsigned char expected[sizeof(symbol)];
	memcpy(expected, &symbol, sizeof(symbol));
	SetLastError(seed);
	TEST_CHECK(!SymFromAddr(session, address, &displacement, &symbol.information));
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_U64_EQ(seed, displacement);
	TEST_CHECK(memcmp(expected, &symbol, sizeof(symbol)) == 0);

	DWORD lineDisplacement = seed;
	IMAGEHLP_LINE64 line;
	memset(&line, 0xa5, sizeof(line));
	line.SizeOfStruct = sizeof(line);
	unsigned char expectedLine[sizeof(line)];
	memcpy(expectedLine, &line, sizeof(line));
	SetLastError(seed);
	TEST_CHECK(!SymGetLineFromAddr64(session, address, &lineDisplacement, &line));
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_EQ(seed, lineDisplacement);
	TEST_CHECK(memcmp(expectedLine, &line, sizeof(line)) == 0);

	SetLastError(seed);
	TEST_CHECK(SymFunctionTableAccess64(session, address) == NULL);
	TEST_CHECK_EQ(error, GetLastError());
}

int main(int argc, char **argv) {
	const DWORD options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
	SetLastError(seed);
	TEST_CHECK_EQ(options, SymSetOptions(options));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(options, SymGetOptions());
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(0, SymSetOptions(0));
	TEST_CHECK_EQ(0, SymGetOptions());
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(options, SymSetOptions(options));

	HANDLE first = (HANDLE)(ULONG_PTR)1;
	HANDLE second = (HANDLE)(ULONG_PTR)2;
	DWORD64 address = (DWORD64)(ULONG_PTR)GetModuleHandleA(NULL);
	check_cleanup(first, FALSE);
	check_cleanup(second, FALSE);
	check_base(first, address, 0, ERROR_INVALID_HANDLE);
	TEST_CHECK(SymInitialize(first, NULL, FALSE));
	TEST_CHECK(SymInitialize(second, "fixture-symbol-path", FALSE));
	check_base(first, address, 0, ERROR_MOD_NOT_FOUND);
	check_base(second, address, 0, ERROR_MOD_NOT_FOUND);

	SetLastError(seed);
	TEST_CHECK(SymInitialize(first, "other-fixture-path", FALSE));
	TEST_CHECK_EQ(seed, GetLastError());
	check_base(first, address, 0, ERROR_MOD_NOT_FOUND);
	check_cleanup(first, TRUE);
	check_cleanup(first, FALSE);
	check_base(first, address, 0, ERROR_INVALID_HANDLE);
	check_base(second, address, 0, ERROR_MOD_NOT_FOUND);
	check_cleanup(second, TRUE);
	TEST_CHECK_EQ(options, SymGetOptions());

	TEST_CHECK(SymInitialize(first, "reset-fixture-path", FALSE));
	check_base(first, address, 0, ERROR_MOD_NOT_FOUND);
	check_cleanup(first, TRUE);

	HANDLE current = GetCurrentProcess();
	TEST_CHECK(SymInitialize(current, NULL, TRUE));
	check_base(current, address, address, seed);
	check_base(current, address + 1, address, seed);
	SetLastError(seed);
	TEST_CHECK(SymInitialize(current, NULL, FALSE));
	TEST_CHECK_EQ(seed, GetLastError());
	check_base(current, address, address, seed);
	check_cleanup(current, TRUE);
	check_base(current, address, 0, ERROR_INVALID_HANDLE);
	TEST_CHECK(SymInitialize(current, NULL, FALSE));
	check_base(current, address, 0, ERROR_MOD_NOT_FOUND);
	check_cleanup(current, TRUE);

	SetLastError(seed);
	TEST_CHECK(!SymInitialize(first, NULL, TRUE));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	check_base(first, address, 0, ERROR_INVALID_HANDLE);
	check_cleanup(first, FALSE);

	if (argc == 2 && strcmp(argv[1], "--unsupported") == 0) {
		SetLastError(seed);
		TEST_CHECK(!SymInitialize(NULL, NULL, FALSE));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
		HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
		TEST_CHECK(process != NULL);
		SetLastError(seed);
		TEST_CHECK(!SymInitialize(process, NULL, TRUE));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		check_cleanup(process, FALSE);
		TEST_CHECK(CloseHandle(process));
		TEST_CHECK(SymInitialize(first, NULL, FALSE));
		check_unsupported_queries(first, address, ERROR_NOT_SUPPORTED);
		check_cleanup(first, TRUE);
		check_unsupported_queries(first, address, ERROR_INVALID_HANDLE);
	}
	return 0;
}
