#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

// These fixtures check one process. Windows global atoms also support other
// processes and survive process termination; wibo does not provide that service.
// API contracts: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globaladdatoma
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globaldeleteatom
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalgetatomnamew
static const DWORD kSentinelError = 0x12345678;

static void make_name(char *name, WCHAR *wide, const char *suffix) {
	sprintf(name, "Wibo.Atoms.%lu.%s", GetCurrentProcessId(), suffix);
	for (size_t i = 0; i <= strlen(name); ++i)
		wide[i] = (unsigned char)name[i];
}

static void check_delete(ATOM atom) {
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalDeleteAtom(atom));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
}

static void test_reference_counts_and_case(void) {
	char name[128], lower[128], result[128];
	WCHAR wide[128], wide_result[128];
	make_name(name, wide, "MiXeD");
	strcpy(lower, name);
	for (size_t i = 0; lower[i]; ++i)
		if (lower[i] >= 'A' && lower[i] <= 'Z')
			lower[i] += 'a' - 'A';
	SetLastError(kSentinelError);
	ATOM atom = GlobalAddAtomA(name);
	TEST_CHECK(atom >= 0xc000);
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(atom, GlobalAddAtomW(wide));
	TEST_CHECK_EQ(atom, GlobalAddAtomA(lower));
	TEST_CHECK_EQ(atom, GlobalFindAtomA(lower));
	TEST_CHECK_EQ(atom, GlobalFindAtomW(wide));
	TEST_CHECK_EQ(strlen(name), GlobalGetAtomNameA(atom, result, sizeof(result)));
	TEST_CHECK_STR_EQ(name, result);
	TEST_CHECK_EQ(wcslen(wide), GlobalGetAtomNameW(atom, wide_result, 128));
	TEST_CHECK_EQ(0, wcscmp(wide, wide_result));
	check_delete(atom);
	check_delete(atom);
	TEST_CHECK_EQ(atom, GlobalFindAtomA(name));
	check_delete(atom);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalFindAtomW(wide));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	SetLastError(kSentinelError);
	// Microsoft documents an always-zero return; Wine 11 returns the bad atom.
	// Both report ERROR_INVALID_HANDLE, which is the portable assertion here.
	GlobalDeleteAtom(atom);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
}

static void test_local_global_separation(void) {
	char local_name[128], global_name[128];
	WCHAR wide[128];
	make_name(local_name, wide, "LocalOnly");
	make_name(global_name, wide, "GlobalOnly");
	TEST_CHECK(AddAtomA(local_name) >= 0xc000);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalFindAtomA(local_name));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	ATOM atom = GlobalAddAtomA(global_name);
	TEST_CHECK(atom >= 0xc000);
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, FindAtomA(global_name));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	check_delete(atom);
}

static void test_integer_atoms(void) {
	char result[32];
	WCHAR wide_result[32];
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(1, GlobalAddAtomA(((LPCSTR)(ULONG_PTR)1)));
	TEST_CHECK_EQ(0xbfff, GlobalFindAtomW(((LPCWSTR)(ULONG_PTR)0xbfff)));
	TEST_CHECK_EQ(123, GlobalAddAtomA("#00123"));
	TEST_CHECK_EQ(123, GlobalFindAtomW(L"#123"));
	TEST_CHECK_EQ(kSentinelError, GetLastError());
	TEST_CHECK_EQ(4, GlobalGetAtomNameA(123, result, sizeof(result)));
	TEST_CHECK_STR_EQ("#123", result);
	TEST_CHECK_EQ(4, GlobalGetAtomNameW(123, wide_result, 32));
	TEST_CHECK_EQ(0, wcscmp(L"#123", wide_result));
	check_delete(123);
	check_delete(0);
	const char *invalid[] = {"#0", "#49152", "#9999999999999999999999999999999"};
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(0, GlobalAddAtomA(invalid[i]));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	}
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalFindAtomA(((LPCSTR)(ULONG_PTR)0xc000)));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	const char *literal[] = {"#", "#+123", "# 123", "#123 ", "#abc"};
	for (unsigned i = 0; i < sizeof(literal) / sizeof(literal[0]); ++i) {
		ATOM atom = GlobalAddAtomA(literal[i]);
		TEST_CHECK(atom >= 0xc000);
		TEST_CHECK_EQ(strlen(literal[i]), GlobalGetAtomNameA(atom, result, sizeof(result)));
		TEST_CHECK_STR_EQ(literal[i], result);
		check_delete(atom);
	}
}

static void test_lengths_and_wide_storage(void) {
	char name[257], result[257];
	WCHAR wide[257], wide_result[257];
	memset(name, 'x', sizeof(name));
	name[255] = 0;
	ATOM atom = GlobalAddAtomA(name);
	TEST_CHECK(atom >= 0xc000);
	TEST_CHECK_EQ(255, GlobalGetAtomNameA(atom, result, sizeof(result)));
	TEST_CHECK_STR_EQ(name, result);
	check_delete(atom);
	name[255] = 'x';
	name[256] = 0;
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalAddAtomA(name));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	for (unsigned i = 0; i < 256; ++i)
		wide[i] = 0x4e2d;
	wide[255] = 0;
	atom = GlobalAddAtomW(wide);
	TEST_CHECK(atom >= 0xc000);
	TEST_CHECK_EQ(255, GlobalGetAtomNameW(atom, wide_result, 257));
	TEST_CHECK_EQ(0, wcscmp(wide, wide_result));
	check_delete(atom);
	wide[255] = 0x4e2d;
	wide[256] = 0;
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalAddAtomW(wide));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(kSentinelError);
	TEST_CHECK_EQ(0, GlobalAddAtomW(L""));
	TEST_CHECK_EQ(ERROR_INVALID_NAME, GetLastError());
	ATOM first = GlobalAddAtomW(L"Wibo.Atom.\x0101");
	ATOM second = GlobalAddAtomW(L"Wibo.Atom.\x0201");
	TEST_CHECK(first >= 0xc000 && second >= 0xc000 && first != second);
	TEST_CHECK_EQ(first, GlobalFindAtomW(L"Wibo.Atom.\x0101"));
	TEST_CHECK_EQ(second, GlobalFindAtomW(L"Wibo.Atom.\x0201"));
	check_delete(first);
	check_delete(second);
}

static void test_current_acp_case(void) {
	// These byte pairs agree in Wine's Windows-1252 and wibo's ISO-8859-1 ACP.
	// Do not assert equivalence for the differing 0x80..0x9f code-page range.
	for (unsigned ch = 0xb5; ch <= 0xff; ++ch) {
		if (!(ch == 0xb5 || (ch >= 0xe0 && ch <= 0xf6) || (ch >= 0xf8 && ch <= 0xff)))
			continue;
		char name[128], upper_name[128], result[128];
		WCHAR wide[128], upper[128], wide_result[128];
		make_name(name, wide, "Latin1.");
		size_t length = strlen(name);
		strcpy(upper_name, name);
		memcpy(upper, wide, (length + 1) * sizeof(WCHAR));
		unsigned upper_ch = ch == 0xb5 ? 0x039c : ch == 0xff ? 0x0178 : ch - 0x20;
		name[length] = (char)ch;
		name[length + 1] = 0;
		wide[length] = ch;
		wide[length + 1] = 0;
		upper[length] = upper_ch;
		upper[length + 1] = 0;
		ATOM atom = GlobalAddAtomA(name);
		TEST_CHECK(atom >= 0xc000);
		TEST_CHECK_EQ(atom, GlobalAddAtomW(wide));
		// Wine's atom table does not fold micro sign to uppercase Greek Mu.
		TEST_CHECK_EQ(ch == 0xb5 ? 0 : atom, GlobalFindAtomW(upper));
		if (upper_ch <= 0xff) {
			upper_name[length] = (char)upper_ch;
			upper_name[length + 1] = 0;
			TEST_CHECK_EQ(atom, GlobalFindAtomA(upper_name));
		}
		TEST_CHECK_EQ(length + 1, GlobalGetAtomNameA(atom, result, 128));
		TEST_CHECK_STR_EQ(name, result);
		TEST_CHECK_EQ(length + 1, GlobalGetAtomNameW(atom, wide_result, 128));
		TEST_CHECK_EQ(0, wcscmp(wide, wide_result));
		check_delete(atom);
		check_delete(atom);
		TEST_CHECK_EQ(0, GlobalFindAtomW(upper));
	}
}

static void test_name_buffers(void) {
	char name[128];
	WCHAR wide[128];
	make_name(name, wide, "Buffers");
	ATOM atom = GlobalAddAtomA(name);
	TEST_CHECK(atom >= 0xc000);
	for (int size = 0; size <= 4; ++size) {
		char result[8];
		WCHAR wide_result[8];
		memset(result, '!', sizeof(result));
		for (unsigned i = 0; i < 8; ++i)
			wide_result[i] = '!';
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(0, GlobalGetAtomNameA(atom, result, size));
		TEST_CHECK_EQ(ERROR_MORE_DATA, GetLastError());
		TEST_CHECK_EQ('!', result[size]);
		if (size > 0) {
			TEST_CHECK_EQ(0, memcmp(result, name, size - 1));
			TEST_CHECK_EQ(0, result[size - 1]);
		}
		SetLastError(kSentinelError);
		TEST_CHECK_EQ(size, GlobalGetAtomNameW(atom, wide_result, size));
		TEST_CHECK_EQ(ERROR_MORE_DATA, GetLastError());
		TEST_CHECK_EQ('!', wide_result[size]);
		TEST_CHECK_EQ(0, memcmp(wide_result, wide, size * sizeof(WCHAR)));
	}
	check_delete(atom);
}

int main(void) {
	test_reference_counts_and_case();
	test_local_global_separation();
	test_integer_atoms();
	test_lengths_and_wide_storage();
	test_current_acp_case();
	test_name_buffers();
	puts("global atom tests passed");
	return 0;
}
