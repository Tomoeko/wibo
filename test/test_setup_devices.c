#include <windows.h>

#include <setupapi.h>

#include "test_assert.h"

static DWORD enumerate(HDEVINFO set, const GUID *classGuid) {
	SP_DEVINFO_DATA device;
	memset(&device, 0xa5, sizeof(device));
	device.cbSize = sizeof(device);
	DWORD count = 0;
	while (TRUE) {
		SetLastError(0x20001234);
		if (!SetupDiEnumDeviceInfo(set, count, &device))
			break;
		TEST_CHECK_EQ(0x20001234, GetLastError());
		TEST_CHECK_EQ(sizeof(device), device.cbSize);
		TEST_CHECK(device.Reserved != 0);
		if (classGuid)
			TEST_CHECK(memcmp(&device.ClassGuid, classGuid, sizeof(GUID)) == 0);
		else if (getenv("WIBO_FIXTURE_PROVIDER"))
			TEST_CHECK_EQ(9 + count, device.DevInst);
		SP_DEVINFO_DATA repeated;
		memset(&repeated, 0, sizeof(repeated));
		repeated.cbSize = sizeof(repeated);
		TEST_CHECK(SetupDiEnumDeviceInfo(set, count, &repeated));
		TEST_CHECK(repeated.Reserved == device.Reserved);
		TEST_CHECK_EQ(device.DevInst, repeated.DevInst);
		TEST_CHECK(memcmp(&device.ClassGuid, &repeated.ClassGuid, sizeof(GUID)) == 0);
		TEST_CHECK(++count < 32768);
	}
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
	return count;
}

int main(void) {
	TEST_CHECK(SetupDiGetClassDevsA(NULL, NULL, NULL, 0) == INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SP_DEVINFO_DATA device;
	memset(&device, 0xa5, sizeof(device));
	device.cbSize = sizeof(device);
	SP_DEVINFO_DATA before = device;
	TEST_CHECK(!SetupDiEnumDeviceInfo(INVALID_HANDLE_VALUE, 0, &device));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(memcmp(&before, &device, sizeof(device)) == 0);
	TEST_CHECK(!SetupDiDestroyDeviceInfoList(INVALID_HANDLE_VALUE));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	HDEVINFO set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	TEST_CHECK(set != INVALID_HANDLE_VALUE);
	TEST_CHECK(!SetupDiEnumDeviceInfo(set, 0, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	device.cbSize = 0;
	before = device;
	TEST_CHECK(!SetupDiEnumDeviceInfo(set, 0, &device));
	TEST_CHECK_EQ(ERROR_INVALID_USER_BUFFER, GetLastError());
	TEST_CHECK(memcmp(&before, &device, sizeof(device)) == 0);
	device.cbSize = sizeof(device);
	before = device;
	TEST_CHECK(!SetupDiEnumDeviceInfo(set, MAXDWORD, &device));
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
	TEST_CHECK(memcmp(&before, &device, sizeof(device)) == 0);
	const DWORD count = enumerate(set, NULL);
	if (getenv("WIBO_FIXTURE_PROVIDER"))
		TEST_CHECK_EQ(2, count);
	if (count) {
		TEST_CHECK(SetupDiEnumDeviceInfo(set, 0, &device));
		const GUID classGuid = device.ClassGuid;
		HDEVINFO filtered = SetupDiGetClassDevsA(&classGuid, NULL, NULL, DIGCF_PRESENT);
		TEST_CHECK(filtered != INVALID_HANDLE_VALUE);
		TEST_CHECK(enumerate(filtered, &classGuid) > 0);
		TEST_CHECK(SetupDiDestroyDeviceInfoList(filtered));
	}
	HDEVINFO empty = SetupDiGetClassDevsA(NULL, "WIBO_SYNTHETIC_ABSENT", NULL, DIGCF_ALLCLASSES);
	TEST_CHECK(empty != INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(0, enumerate(empty, NULL));
	TEST_CHECK(SetupDiDestroyDeviceInfoList(empty));
	SetLastError(0x20001234);
	TEST_CHECK(SetupDiDestroyDeviceInfoList(set));
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK(!SetupDiEnumDeviceInfo(set, 0, &device));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
		TEST_CHECK(!SetupDiDestroyDeviceInfoList(set));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
		TEST_CHECK(SetupDiGetClassDevsA(NULL, NULL, (HWND)(ULONG_PTR)1, DIGCF_ALLCLASSES) == INVALID_HANDLE_VALUE);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		const char *faults[] = {"truncated", "trailing", "bad-guid", "bad-count", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_DEVICES_RESPONSE", faults[i]));
			TEST_CHECK(SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES) == INVALID_HANDLE_VALUE);
			TEST_CHECK_EQ(i == 4 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA, GetLastError());
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_DEVICES_RESPONSE", NULL));
		char provider[32768];
		TEST_CHECK(GetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", provider, sizeof(provider)) > 0);
		TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", NULL));
		TEST_CHECK(SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES) == INVALID_HANDLE_VALUE);
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", provider));
	}
	return 0;
}
