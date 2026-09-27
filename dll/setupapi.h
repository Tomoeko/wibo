#pragma once

#include "types.h"

namespace setupapi {

struct SP_DEVINFO_DATA {
	DWORD cbSize;
	GUID ClassGuid;
	DWORD DevInst;
	GUEST_PTR Reserved;
};

HANDLE WINAPI SetupDiGetClassDevsA(const GUID *classGuid, LPCSTR enumerator, HWND parent, DWORD flags);
BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE deviceInfoSet, DWORD memberIndex, SP_DEVINFO_DATA *deviceInfoData);
BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE deviceInfoSet);

} // namespace setupapi
