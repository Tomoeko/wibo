#pragma once

#include "types.h"

namespace shlwapi {

BOOL WINAPI PathIsDirectoryW(LPCWSTR path);
BOOL WINAPI PathCanonicalizeW(LPWSTR output, LPCWSTR path);
BOOL WINAPI PathMatchSpecW(LPCWSTR file, LPCWSTR pattern);
LPSTR WINAPI PathAddBackslashA(LPSTR pszPath);

} // namespace shlwapi
