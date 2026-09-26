#pragma once

#include "types.h"

namespace ole32 {

HRESULT WINAPI CoSetProxyBlanket(GUEST_PTR proxy, DWORD authentication, DWORD authorization, LPCWSTR principal,
	DWORD level, DWORD impersonation, GUEST_PTR identity, DWORD capabilities);
HRESULT WINAPI CoCreateGuid(GUID *pguid);
PVOID WINAPI CoTaskMemAlloc(SIZE_T cb);
void WINAPI CoTaskMemFree(PVOID pv);

HRESULT WINAPI CoInitialize(LPVOID pvReserved);
HRESULT WINAPI CoInitializeEx(LPVOID pvReserved, DWORD flags);
void WINAPI CoUninitialize();
HRESULT WINAPI CoCreateInstance(const GUID *rclsid, LPVOID pUnkOuter, DWORD dwClsContext, const GUID *riid, GUEST_PTR *ppv);
HRESULT WINAPI CLSIDFromString(LPCWSTR lpsz, GUID *pclsid);

} // namespace ole32
