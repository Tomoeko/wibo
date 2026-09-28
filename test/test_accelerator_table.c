#define _WIN32_WINNT 0x0601
#include <string.h>
#include <windows.h>

#include "test_assert.h"

static void check_entry(const ACCEL *entry, BYTE flags, WORD key, WORD command) {
	TEST_CHECK_EQ(flags, entry->fVirt);
	TEST_CHECK_EQ(key, entry->key);
	TEST_CHECK_EQ(command, entry->cmd);
}

int main(int argc, char **argv) {
	const DWORD seed = 4321;
	ACCEL input[2] = {{FVIRTKEY | FCONTROL, 'A', 101}, {FALT, 'q', 202}};
	ACCEL copy[3];
	HACCEL table;
	MSG message;
	(void)argv;

	if (argc > 1) {
		ACCEL high = {0, 0x80, 501};
		ACCEL wide = {0, 0x20ac, 502};
		SetLastError(seed);
		TEST_CHECK(CreateAcceleratorTableA(&high, 1) == NULL);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		table = CreateAcceleratorTableW(&wide, 1);
		TEST_CHECK(table != NULL);
		memset(copy, 0xa5, sizeof(copy));
		SetLastError(seed);
		TEST_CHECK_EQ(0, CopyAcceleratorTableA(table, copy, 1));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK_EQ(0xa5, copy[0].fVirt);
		memset(copy, 0xa5, sizeof(copy));
		SetLastError(seed);
		TEST_CHECK_EQ(0, CopyAcceleratorTableW(table, copy, -1));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		TEST_CHECK_EQ(0xa5, copy[0].fVirt);
		TEST_CHECK(DestroyAcceleratorTable(table));
		return 0;
	}

	SetLastError(seed);
	table = CreateAcceleratorTableA(input, 2);
	TEST_CHECK(table != NULL);
	TEST_CHECK_EQ(seed, GetLastError());
	input[0].cmd = 999;
	SetLastError(seed);
	TEST_CHECK_EQ(2, CopyAcceleratorTableA(table, NULL, 0));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK_EQ(2, CopyAcceleratorTableW(table, NULL, -1));
	TEST_CHECK_EQ(seed, GetLastError());
	memset(copy, 0xa5, sizeof(copy));
	SetLastError(seed);
	TEST_CHECK_EQ(1, CopyAcceleratorTableA(table, copy, 1));
	TEST_CHECK_EQ(seed, GetLastError());
	check_entry(&copy[0], FVIRTKEY | FCONTROL, 'A', 101);
	TEST_CHECK_EQ(0xa5, copy[1].fVirt);
	memset(copy, 0xa5, sizeof(copy));
	SetLastError(seed);
	TEST_CHECK_EQ(2, CopyAcceleratorTableW(table, copy, 3));
	TEST_CHECK_EQ(seed, GetLastError());
	check_entry(&copy[0], FVIRTKEY | FCONTROL, 'A', 101);
	check_entry(&copy[1], FALT, 'q', 202);
	TEST_CHECK_EQ(0xa5, copy[2].fVirt);
	memset(copy, 0xa5, sizeof(copy));
	SetLastError(seed);
	TEST_CHECK_EQ(0, CopyAcceleratorTableW(table, copy, 0));
	TEST_CHECK_EQ(0xa5, copy[0].fVirt);
	TEST_CHECK_EQ(seed, GetLastError());
	memset(&message, 0, sizeof(message));
	message.message = WM_KEYDOWN;
	message.wParam = 'A';
	SetLastError(seed);
	TEST_CHECK_EQ(0, TranslateAcceleratorA(NULL, table, &message));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK_EQ(0, TranslateAcceleratorW(NULL, table, &message));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(WM_KEYDOWN, message.message);
	TEST_CHECK_EQ('A', message.wParam);

	SetLastError(seed);
	TEST_CHECK(DestroyAcceleratorTable(table));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK(!DestroyAcceleratorTable(table));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK_EQ(0, CopyAcceleratorTableW(table, NULL, 0));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK(!DestroyAcceleratorTable(NULL));
	TEST_CHECK_EQ(seed, GetLastError());
	SetLastError(seed);
	TEST_CHECK(CreateAcceleratorTableW(input, 0) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(seed);
	TEST_CHECK(CreateAcceleratorTableW(input, -1) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());

	input[0].cmd = 101;
	table = CreateAcceleratorTableW(input, 2);
	TEST_CHECK(table != NULL);
	memset(copy, 0xa5, sizeof(copy));
	TEST_CHECK_EQ(2, CopyAcceleratorTableA(table, copy, 2));
	check_entry(&copy[0], FVIRTKEY | FCONTROL, 'A', 101);
	check_entry(&copy[1], FALT, 'q', 202);
	TEST_CHECK(DestroyAcceleratorTable(table));
	return 0;
}
