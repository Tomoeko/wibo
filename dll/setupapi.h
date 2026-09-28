#pragma once

#include "types.h"

namespace setupapi {

struct SP_DEVINFO_DATA {
	DWORD cbSize;
	GUID ClassGuid;
	DWORD DevInst;
	GUEST_PTR Reserved;
};

struct SP_DEVICE_INTERFACE_DATA {
	DWORD cbSize;
	GUID InterfaceClassGuid;
	DWORD Flags;
	GUEST_PTR Reserved;
};

struct SP_DEVICE_INTERFACE_DETAIL_DATA_A {
	DWORD cbSize;
	CHAR DevicePath[1];
};

struct SP_DEVICE_INTERFACE_DETAIL_DATA_W {
	DWORD cbSize;
	WCHAR DevicePath[1];
};

HANDLE WINAPI SetupDiGetClassDevsA(const GUID *classGuid, LPCSTR enumerator, HWND parent, DWORD flags);
BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE deviceInfoSet, DWORD memberIndex, SP_DEVINFO_DATA *deviceInfoData);
BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE deviceInfoSet, SP_DEVINFO_DATA *deviceInfoData,
		const GUID *interfaceClassGuid, DWORD memberIndex, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData);
BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HANDLE deviceInfoSet, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData,
		SP_DEVICE_INTERFACE_DETAIL_DATA_A *deviceInterfaceDetailData, DWORD deviceInterfaceDetailDataSize,
		DWORD *requiredSize, SP_DEVINFO_DATA *deviceInfoData);
BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE deviceInfoSet, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData,
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *deviceInterfaceDetailData, DWORD deviceInterfaceDetailDataSize,
		DWORD *requiredSize, SP_DEVINFO_DATA *deviceInfoData);
BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE deviceInfoSet);

} // namespace setupapi
