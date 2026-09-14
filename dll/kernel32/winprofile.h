#pragma once

#include "types.h"

namespace kernel32 {

UINT WINAPI GetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault, LPCSTR lpFileName);
UINT WINAPI GetPrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nDefault, LPCWSTR lpFileName);
DWORD WINAPI GetPrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
									  DWORD nSize, LPCSTR lpFileName);
DWORD WINAPI GetPrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault, LPWSTR lpReturnedString,
									  DWORD nSize, LPCWSTR lpFileName);
UINT WINAPI GetProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault);
UINT WINAPI GetProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nDefault);
DWORD WINAPI GetProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
							   DWORD nSize);
DWORD WINAPI GetProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault, LPWSTR lpReturnedString,
							   DWORD nSize);

} // namespace kernel32
