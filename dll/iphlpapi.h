#pragma once

#include "types.h"

struct MIB_IPADDRROW {
	DWORD dwAddr, dwIndex, dwMask, dwBCastAddr, dwReasmSize;
	WORD unused, wType;
};
static_assert(sizeof(MIB_IPADDRROW) == 24);

namespace iphlpapi {
ULONG WINAPI GetBestRoute2(const ULONGLONG *luid, ULONG index, LPCVOID source, LPCVOID destination, ULONG options,
						   LPVOID route, LPVOID bestSource);
ULONG WINAPI GetAdaptersAddresses(ULONG family, ULONG flags, LPVOID reserved, LPVOID addresses, ULONG *size);
DWORD WINAPI GetIpAddrTable(LPVOID table, ULONG *size, BOOL ordered);
} // namespace iphlpapi
