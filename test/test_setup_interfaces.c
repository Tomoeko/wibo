#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <setupapi.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

static const GUID interfaceClass = {0x03020100, 0x0504, 0x0706, {8, 9, 10, 11, 12, 13, 14, 15}};
static const GUID ownerClass = {0x13121110, 0x1514, 0x1716, {24, 25, 26, 27, 28, 29, 30, 31}};

int main(int argc, char **argv) {
	if (argc == 2) {
		TEST_CHECK(getenv("WIBO_FIXTURE_PROVIDER") != NULL);
		TEST_CHECK(SetupDiGetClassDevsA(&interfaceClass, NULL, NULL,
			DIGCF_PRESENT | DIGCF_DEVICEINTERFACE) == INVALID_HANDLE_VALUE);
		TEST_CHECK_EQ(strcmp(argv[1], "failed") == 0 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA,
			GetLastError());
		return 0;
	}
	TEST_CHECK_EQ(1, argc);
	const BOOL fixture = getenv("WIBO_FIXTURE_PROVIDER") != NULL;
	SP_DEVICE_INTERFACE_DATA invalid;
	memset(&invalid, 0xa5, sizeof(invalid));
	invalid.cbSize = sizeof(invalid);
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(INVALID_HANDLE_VALUE, NULL, &interfaceClass, 0, &invalid));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(sizeof(invalid), invalid.cbSize);
	TEST_CHECK_EQ(0, invalid.InterfaceClassGuid.Data1);
	TEST_CHECK_EQ(0, invalid.Flags);
	TEST_CHECK_EQ(0, invalid.Reserved);
	HDEVINFO set = SetupDiGetClassDevsA(&interfaceClass, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	TEST_CHECK(set != INVALID_HANDLE_VALUE);
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, 0, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	memset(&invalid, 0xa5, sizeof(invalid));
	invalid.cbSize = 0;
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, 0, &invalid));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(0xa5a5a5a5, invalid.Flags);
	invalid.cbSize = sizeof(invalid);
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, MAXDWORD, &invalid));
	TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
	TEST_CHECK_EQ(0, invalid.Flags);
	TEST_CHECK_EQ(0, invalid.Reserved);
	SP_DEVINFO_DATA unknown = {0};
	unknown.cbSize = sizeof(unknown);
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, &unknown, &interfaceClass, 0, &invalid));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	unknown.cbSize = 0;
	TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, &unknown, &interfaceClass, 0, &invalid));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());

	if (fixture) {
		SP_DEVICE_INTERFACE_DATA interfaceData = {0};
		interfaceData.cbSize = sizeof(interfaceData);
		SetLastError(0x20001234);
		TEST_CHECK(SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, 0, &interfaceData));
		TEST_CHECK_EQ(0x20001234, GetLastError());
		TEST_CHECK_EQ(sizeof(interfaceData), interfaceData.cbSize);
		TEST_CHECK(!memcmp(&interfaceData.InterfaceClassGuid, &interfaceClass, sizeof(GUID)));
		TEST_CHECK_EQ(1, interfaceData.Flags);
		TEST_CHECK(interfaceData.Reserved != 0);
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, 1, &invalid));
		TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, NULL, 0, &invalid));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		SP_DEVINFO_DATA owner = {0};
		owner.cbSize = sizeof(owner);
		TEST_CHECK(SetupDiEnumDeviceInfo(set, 0, &owner));
		TEST_CHECK(!memcmp(&owner.ClassGuid, &ownerClass, sizeof(GUID)));
		TEST_CHECK_EQ(9, owner.DevInst);
		SP_DEVICE_INTERFACE_DATA filtered = {0};
		filtered.cbSize = sizeof(filtered);
		TEST_CHECK(SetupDiEnumDeviceInterfaces(set, &owner, &interfaceClass, 0, &filtered));
		TEST_CHECK(filtered.Reserved == interfaceData.Reserved);
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, &owner, &interfaceClass, 1, &filtered));
		TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
		HDEVINFO other = SetupDiGetClassDevsA(&interfaceClass, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		TEST_CHECK(other != INVALID_HANDLE_VALUE);
		SP_DEVINFO_DATA otherOwner = {0};
		otherOwner.cbSize = sizeof(otherOwner);
		TEST_CHECK(SetupDiEnumDeviceInfo(other, 0, &otherOwner));
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, &otherOwner, &interfaceClass, 0, &filtered));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		TEST_CHECK(SetupDiDestroyDeviceInfoList(other));

		static const WCHAR expectedWide[] = L"\\\\?\\SYNTHETIC#DEVICE#0";
		static const char expectedNarrow[] = "\\\\?\\SYNTHETIC#DEVICE#0";
		DWORD required = 0;
		SP_DEVINFO_DATA detailOwner = {0};
		detailOwner.cbSize = sizeof(detailOwner);
		TEST_CHECK(!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, NULL, 0, &required, &detailOwner));
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
		TEST_CHECK_EQ(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(expectedWide), required);
		unsigned char detailBuffer[256] = {0};
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detailWide = (void *)detailBuffer;
		detailWide->cbSize = sizeof(*detailWide);
		TEST_CHECK(SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detailWide, sizeof(detailBuffer), &required, &detailOwner));
		TEST_CHECK(!memcmp(detailWide->DevicePath, expectedWide, sizeof(expectedWide)));
		TEST_CHECK_EQ(9, detailOwner.DevInst);
		TEST_CHECK(!memcmp(&detailOwner.ClassGuid, &ownerClass, sizeof(GUID)));
		TEST_CHECK(detailOwner.Reserved == owner.Reserved);
		SP_DEVICE_INTERFACE_DETAIL_DATA_A *detailNarrow = (void *)detailBuffer;
		detailNarrow->cbSize = sizeof(*detailNarrow);
		TEST_CHECK(!SetupDiGetDeviceInterfaceDetailA(set, &interfaceData, NULL, 0, &required, NULL));
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
		TEST_CHECK_EQ(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof(expectedNarrow), required);
		TEST_CHECK(SetupDiGetDeviceInterfaceDetailA(set, &interfaceData, detailNarrow, sizeof(detailBuffer), &required, NULL));
		TEST_CHECK(!memcmp(detailNarrow->DevicePath, expectedNarrow, sizeof(expectedNarrow)));
		detailNarrow->cbSize = 0;
		TEST_CHECK(!SetupDiGetDeviceInterfaceDetailA(set, &interfaceData, detailNarrow, sizeof(detailBuffer), &required, NULL));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		TEST_CHECK(SetupDiDestroyDeviceInfoList(set));
		TEST_CHECK(!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, NULL, 0, &required, NULL));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	} else {
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, &interfaceClass, 0, &invalid));
		TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
		TEST_CHECK(!SetupDiEnumDeviceInterfaces(set, NULL, NULL, 0, &invalid));
		TEST_CHECK_EQ(ERROR_NO_MORE_ITEMS, GetLastError());
		TEST_CHECK(SetupDiDestroyDeviceInfoList(set));
	}
	return 0;
}
