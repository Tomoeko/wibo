#include <windows.h>

#include "test_assert.h"

// Relative comparisons deliberately use the process-visible version: Wine and
// wibo need not report the same OS, build, service pack, or product suite.
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-verifyversioninfow
// https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-versetconditionmask
typedef ULONGLONG(WINAPI *SetConditionMaskFn)(ULONGLONG, DWORD, BYTE);

static OSVERSIONINFOEXA current_a;
static OSVERSIONINFOEXW current_w;
static unsigned int comparisons;
static const DWORD last_error_sentinel = 0x13572468;
static const DWORD hierarchy = VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR;

static ULONGLONG conditions(DWORD fields, BYTE condition) {
	ULONGLONG mask = 0;
	for (unsigned int bit = 0; bit < 8; ++bit) {
		if (fields & (1u << bit)) {
			mask = VerSetConditionMask(mask, 1u << bit, condition);
		}
	}
	return mask;
}

static void check_version(const char *name, OSVERSIONINFOEXA requested, DWORD fields, ULONGLONG mask, BOOL expected,
						  DWORD expected_error) {
	OSVERSIONINFOEXA before_a = requested;
	OSVERSIONINFOEXW wide = current_w;
	wide.dwMajorVersion = requested.dwMajorVersion;
	wide.dwMinorVersion = requested.dwMinorVersion;
	wide.dwBuildNumber = requested.dwBuildNumber;
	wide.dwPlatformId = requested.dwPlatformId;
	wide.wServicePackMajor = requested.wServicePackMajor;
	wide.wServicePackMinor = requested.wServicePackMinor;
	wide.wSuiteMask = requested.wSuiteMask;
	wide.wProductType = requested.wProductType;
	OSVERSIONINFOEXW before_w = wide;
	SetLastError(last_error_sentinel);
	BOOL result = VerifyVersionInfoA(&requested, fields, mask);
	DWORD error = GetLastError();
	TEST_CHECK_MSG(!!result == !!expected && error == expected_error, "%s A: got %d/error %lu, expected %d/error %lu",
				   name, result, error, expected, expected_error);
	TEST_CHECK_EQ(0, memcmp(&before_a, &requested, sizeof(requested)));
	SetLastError(last_error_sentinel);
	result = VerifyVersionInfoW(&wide, fields, mask);
	error = GetLastError();
	TEST_CHECK_MSG(!!result == !!expected && error == expected_error, "%s W: got %d/error %lu, expected %d/error %lu",
				   name, result, error, expected, expected_error);
	TEST_CHECK_EQ(0, memcmp(&before_w, &wide, sizeof(wide)));
	comparisons += 2;
}

static void check(const char *name, OSVERSIONINFOEXA requested, DWORD fields, ULONGLONG mask, BOOL expected) {
	check_version(name, requested, fields, mask, expected, expected ? last_error_sentinel : ERROR_OLD_WIN_VERSION);
}

static void test_masks(void) {
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(ntdll != NULL);
	SetConditionMaskFn native_set = (SetConditionMaskFn)(void *)GetProcAddress(ntdll, "VerSetConditionMask");
	TEST_CHECK(native_set != NULL);
	const ULONGLONG high_bits = 0xa5a55a5a00000000ULL;
	for (unsigned int bit = 0; bit < 8; ++bit) {
		for (BYTE op = VER_EQUAL; op <= VER_OR; ++op) {
			ULONGLONG expected = high_bits | ((ULONGLONG)op << (3 * bit));
			SetLastError(last_error_sentinel);
			TEST_CHECK_U64_EQ(expected, VerSetConditionMask(high_bits, 1u << bit, op));
			TEST_CHECK_EQ(last_error_sentinel, GetLastError());
			TEST_CHECK_U64_EQ(expected, native_set(high_bits, 1u << bit, op));
			TEST_CHECK_EQ(last_error_sentinel, GetLastError());
		}
	}
	// The routine accumulates bits, rather than replacing an existing operator.
	ULONGLONG mask = VerSetConditionMask(high_bits, VER_MAJORVERSION, VER_GREATER);
	TEST_CHECK_U64_EQ(high_bits | ((ULONGLONG)VER_GREATER_EQUAL << 3),
					  VerSetConditionMask(mask, VER_MAJORVERSION, VER_EQUAL));
	TEST_CHECK_U64_EQ(high_bits, VerSetConditionMask(high_bits, 0, VER_EQUAL));
	TEST_CHECK_U64_EQ(high_bits, native_set(high_bits, 0, VER_EQUAL));
	// Multiple field bits select the highest recognized bit; documented callers
	// call once per field, as conditions() above does.
	TEST_CHECK_U64_EQ(high_bits | ((ULONGLONG)VER_EQUAL << 21),
					  VerSetConditionMask(high_bits, VER_MAJORVERSION | VER_PRODUCT_TYPE, VER_EQUAL));
	TEST_CHECK_U64_EQ(high_bits | ((ULONGLONG)VER_EQUAL << 21),
					  native_set(high_bits, VER_MAJORVERSION | VER_PRODUCT_TYPE, VER_EQUAL));
}

static DWORD scalar_value(const OSVERSIONINFOEXA *info, DWORD field) {
	switch (field) {
	case VER_MAJORVERSION:
		return info->dwMajorVersion;
	case VER_MINORVERSION:
		return info->dwMinorVersion;
	case VER_BUILDNUMBER:
		return info->dwBuildNumber;
	case VER_PLATFORMID:
		return info->dwPlatformId;
	case VER_SERVICEPACKMAJOR:
		return info->wServicePackMajor;
	case VER_SERVICEPACKMINOR:
		return info->wServicePackMinor;
	case VER_PRODUCT_TYPE:
		return info->wProductType;
	default:
		TEST_FAIL("Unexpected test field");
	}
}

static void set_scalar(OSVERSIONINFOEXA *info, DWORD field, DWORD value) {
	switch (field) {
	case VER_MAJORVERSION:
		info->dwMajorVersion = value;
		break;
	case VER_MINORVERSION:
		info->dwMinorVersion = value;
		break;
	case VER_BUILDNUMBER:
		info->dwBuildNumber = value;
		break;
	case VER_PLATFORMID:
		info->dwPlatformId = value;
		break;
	case VER_SERVICEPACKMAJOR:
		info->wServicePackMajor = (WORD)value;
		break;
	case VER_SERVICEPACKMINOR:
		info->wServicePackMinor = (WORD)value;
		break;
	case VER_PRODUCT_TYPE:
		info->wProductType = (BYTE)value;
		break;
	default:
		TEST_FAIL("Unexpected test field");
	}
}

static void test_scalars(void) {
	const DWORD fields[] = {VER_MAJORVERSION,	  VER_MINORVERSION,		VER_BUILDNUMBER, VER_PLATFORMID,
							VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR, VER_PRODUCT_TYPE};
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		DWORD field = fields[i], value = scalar_value(&current_a, field);
		DWORD maximum = field == VER_PRODUCT_TYPE
							? 255
							: (field == VER_SERVICEPACKMAJOR || field == VER_SERVICEPACKMINOR ? 65535 : MAXDWORD);
		for (BYTE op = VER_EQUAL; op <= VER_LESS_EQUAL; ++op) {
			ULONGLONG mask = conditions(field, op);
			check("equal scalar", current_a, field, mask,
				  op == VER_EQUAL || op == VER_GREATER_EQUAL || op == VER_LESS_EQUAL);
			OSVERSIONINFOEXA requested = current_a;
			if (value != maximum) {
				set_scalar(&requested, field, value + 1);
				check("higher requested scalar", requested, field, mask, op == VER_LESS || op == VER_LESS_EQUAL);
			}
			if (value != 0) {
				set_scalar(&requested, field, value - 1);
				check("lower requested scalar", requested, field, mask, op == VER_GREATER || op == VER_GREATER_EQUAL);
			}
		}
	}
}

static void test_hierarchy(void) {
	check("hierarchy equality", current_a, hierarchy, conditions(hierarchy, VER_EQUAL), TRUE);
	check("hierarchy inclusive equality", current_a, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL), TRUE);
	check("hierarchy strict equality", current_a, hierarchy, conditions(hierarchy, VER_GREATER), FALSE);
	check("hierarchy strict less equality", current_a, hierarchy, conditions(hierarchy, VER_LESS), FALSE);
	OSVERSIONINFOEXA requested = current_a;
	TEST_CHECK(current_a.dwMajorVersion > 0 && current_a.dwMajorVersion < MAXDWORD);
	TEST_CHECK(current_a.dwMinorVersion < MAXDWORD);
	TEST_CHECK(current_a.wServicePackMajor < 65535 && current_a.wServicePackMinor < 65535);
	requested.dwMajorVersion--;
	requested.dwMinorVersion = MAXDWORD;
	requested.wServicePackMajor = requested.wServicePackMinor = 65535;
	check("major wins over later higher requirements", requested, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL),
		  TRUE);
	check("major rejects before later fields", requested, hierarchy, conditions(hierarchy, VER_LESS_EQUAL), FALSE);
	requested = current_a;
	requested.dwMajorVersion++;
	requested.dwMinorVersion = 0;
	requested.wServicePackMajor = requested.wServicePackMinor = 0;
	check("lower major ignores later fields", requested, hierarchy, conditions(hierarchy, VER_LESS_EQUAL), TRUE);
	check("higher required major rejects", requested, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL), FALSE);
	requested = current_a;
	requested.dwMinorVersion++;
	check("equal major lower minor", requested, hierarchy, conditions(hierarchy, VER_LESS_EQUAL), TRUE);
	check("equal major rejects higher minor", requested, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL), FALSE);
	ULONGLONG mixed = conditions(VER_MAJORVERSION, VER_GREATER) | conditions(VER_MINORVERSION, VER_LESS_EQUAL);
	check("major greater overrides minor less", requested, VER_MAJORVERSION | VER_MINORVERSION, mixed, FALSE);
	mixed = conditions(VER_MAJORVERSION, VER_LESS) | conditions(VER_MINORVERSION, VER_GREATER_EQUAL);
	check("major less overrides minor greater", requested, VER_MAJORVERSION | VER_MINORVERSION, mixed, TRUE);
	mixed = conditions(VER_MAJORVERSION, VER_EQUAL) | conditions(VER_MINORVERSION, VER_LESS_EQUAL);
	check("major equality permits minor operator", requested, VER_MAJORVERSION | VER_MINORVERSION, mixed, TRUE);
	requested = current_a;
	requested.wServicePackMajor++;
	requested.wServicePackMinor = 0;
	check("service pack major comparison", requested, hierarchy, conditions(hierarchy, VER_LESS_EQUAL), TRUE);
	check("service pack major rejection", requested, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL), FALSE);
	requested = current_a;
	requested.wServicePackMinor++;
	check("service pack minor comparison", requested, hierarchy, conditions(hierarchy, VER_LESS_EQUAL), TRUE);
	check("service pack minor rejection", requested, hierarchy, conditions(hierarchy, VER_GREATER_EQUAL), FALSE);
	mixed = conditions(VER_MAJORVERSION, VER_GREATER_EQUAL) | conditions(VER_MINORVERSION, VER_GREATER);
	check("strict last selected component", current_a, VER_MAJORVERSION | VER_MINORVERSION, mixed, FALSE);
	mixed |= conditions(VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, VER_GREATER_EQUAL);
	check("equal components continue to final operator", current_a, hierarchy, mixed, TRUE);
	mixed = conditions(VER_MAJORVERSION, VER_GREATER_EQUAL) | conditions(VER_SERVICEPACKMINOR, VER_GREATER);
	check("missing intermediate operator retains governing condition", current_a, hierarchy, mixed, TRUE);
	if (current_a.dwBuildNumber < MAXDWORD) {
		requested = current_a;
		requested.dwMajorVersion--;
		requested.dwBuildNumber++;
		mixed = conditions(hierarchy, VER_GREATER_EQUAL) | conditions(VER_BUILDNUMBER, VER_EQUAL);
		check("independent build still rejects", requested, hierarchy | VER_BUILDNUMBER, mixed, FALSE);
	}
}

static void test_suites_and_errors(void) {
	OSVERSIONINFOEXA requested = current_a;
	check("all current suites", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_AND), TRUE);
	check("any current suite", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_OR), TRUE);
	requested.wSuiteMask = 0;
	check("empty suite AND", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_AND), TRUE);
	check("empty suite OR", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_OR), TRUE);
	for (unsigned int bit = 0; bit < 16; ++bit) {
		if (!(current_a.wSuiteMask & (1u << bit))) {
			requested.wSuiteMask = (WORD)(1u << bit);
			check("absent suite AND", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_AND), FALSE);
			check("absent suite OR", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_OR), FALSE);
			requested.wSuiteMask |= current_a.wSuiteMask;
			check("mixed suites AND", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_AND), FALSE);
			check("mixed suites OR", requested, VER_SUITENAME, conditions(VER_SUITENAME, VER_OR),
				  current_a.wSuiteMask != 0);
			break;
		}
	}
	check_version("empty type mask", current_a, 0, conditions(VER_MAJORVERSION, VER_EQUAL), FALSE, ERROR_BAD_ARGUMENTS);
	check_version("empty condition mask", current_a, VER_MAJORVERSION, 0, FALSE, ERROR_BAD_ARGUMENTS);
	check_version("invalid suite operator", current_a, VER_SUITENAME, conditions(VER_SUITENAME, VER_EQUAL), FALSE,
				  ERROR_BAD_ARGUMENTS);
	check("invalid scalar operator", current_a, VER_BUILDNUMBER, conditions(VER_BUILDNUMBER, VER_AND), FALSE);
	check("missing selected scalar operator", current_a, VER_BUILDNUMBER, conditions(VER_MAJORVERSION, VER_EQUAL),
		  FALSE);
	requested = current_a;
	requested.dwMajorVersion = requested.dwMinorVersion = MAXDWORD;
	requested.wServicePackMajor = requested.wServicePackMinor = 65535;
	requested.szCSDVersion[0] = 'X';
	check("unselected fields ignored", requested, VER_BUILDNUMBER, conditions(VER_BUILDNUMBER, VER_EQUAL), TRUE);
}

int main(void) {
	current_a.dwOSVersionInfoSize = sizeof(current_a);
	current_w.dwOSVersionInfoSize = sizeof(current_w);
	TEST_CHECK(GetVersionExA((OSVERSIONINFOA *)&current_a));
	TEST_CHECK(GetVersionExW((OSVERSIONINFOW *)&current_w));
	TEST_CHECK_EQ(current_a.dwMajorVersion, current_w.dwMajorVersion);
	TEST_CHECK_EQ(current_a.dwMinorVersion, current_w.dwMinorVersion);
	TEST_CHECK_EQ(current_a.dwBuildNumber, current_w.dwBuildNumber);
	TEST_CHECK_EQ(current_a.wServicePackMajor, current_w.wServicePackMajor);
	TEST_CHECK_EQ(current_a.wServicePackMinor, current_w.wServicePackMinor);
	TEST_CHECK_EQ(current_a.wSuiteMask, current_w.wSuiteMask);
	TEST_CHECK_EQ(current_a.wProductType, current_w.wProductType);
	test_masks();
	test_scalars();
	test_hierarchy();
	test_suites_and_errors();
	printf(
		"version condition tests passed: %u A/W comparisons; visible version %lu.%lu.%lu SP%u.%u suite%u product%u\n",
		comparisons, current_a.dwMajorVersion, current_a.dwMinorVersion, current_a.dwBuildNumber,
		current_a.wServicePackMajor, current_a.wServicePackMinor, current_a.wSuiteMask, current_a.wProductType);
	return 0;
}
