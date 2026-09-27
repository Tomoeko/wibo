#pragma once

#include "types.h"

namespace shell32 {

GUEST_PTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
HINSTANCE WINAPI FindExecutableW(LPCWSTR file, LPCWSTR directory, LPWSTR result);
HRESULT WINAPI SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, GUEST_PTR *output);
HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath);

} // namespace shell32
