#pragma once

#include "types.h"

namespace shell32 {

GUEST_PTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs);
HRESULT WINAPI SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, GUEST_PTR *output);
HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath);

} // namespace shell32
