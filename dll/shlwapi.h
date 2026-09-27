#pragma once

#include "types.h"

namespace shlwapi {

BOOL WINAPI PathIsDirectoryW(LPCWSTR path);
BOOL WINAPI PathCanonicalizeW(LPWSTR output, LPCWSTR path);
LPSTR WINAPI PathAddBackslashA(LPSTR pszPath);

} // namespace shlwapi
