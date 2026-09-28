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
HRESULT WINAPI CoGetContextToken(ULONG_PTR *token);
HRESULT WINAPI OleInitialize(LPVOID pvReserved);
void WINAPI OleUninitialize();
HRESULT WINAPI CoCreateInstance(const GUID *rclsid, LPVOID pUnkOuter, DWORD dwClsContext, const GUID *riid,
								GUEST_PTR *ppv);
HRESULT WINAPI CLSIDFromString(LPCWSTR lpsz, GUID *pclsid);
int WINAPI StringFromGUID2(const GUID *guid, LPWSTR output, int capacity);
HRESULT WINAPI StringFromCLSID(const GUID *guid, GUEST_PTR *output);
HRESULT WINAPI StringFromIID(const GUID *guid, GUEST_PTR *output);
HRESULT WINAPI IIDFromString(LPCWSTR text, GUID *guid);

} // namespace ole32
