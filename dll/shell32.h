#pragma once

#include "types.h"

namespace shell32 {

struct SHFILEOPSTRUCTW {
	HWND hwnd;
	UINT wFunc;
	GUEST_PTR pFrom;
	GUEST_PTR pTo;
	WORD fFlags;
	BOOL fAnyOperationsAborted;
	GUEST_PTR hNameMappings;
	GUEST_PTR lpszProgressTitle;
};
#ifdef WIBO_GUEST_64
static_assert(sizeof(SHFILEOPSTRUCTW) == 56);
#else
static_assert(sizeof(SHFILEOPSTRUCTW) == 32);
#endif

struct SHFILEINFOW {
	HANDLE hIcon;
	int iIcon;
	DWORD dwAttributes;
	WCHAR szDisplayName[260];
	WCHAR szTypeName[80];
};
#ifdef WIBO_GUEST_64
static_assert(sizeof(SHFILEINFOW) == 696);
#else
static_assert(sizeof(SHFILEINFOW) == 692);
#endif

GUEST_PTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
int WINAPI SHFileOperationW(SHFILEOPSTRUCTW *operation);
HINSTANCE WINAPI FindExecutableW(LPCWSTR file, LPCWSTR directory, LPWSTR result);
DWORD_PTR WINAPI SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags);
HRESULT WINAPI SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, GUEST_PTR *output);
HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath);

} // namespace shell32
