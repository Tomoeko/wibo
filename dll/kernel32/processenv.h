#pragma once

#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kernel32 {
std::optional<std::u16string> environmentValue(std::u16string_view name);

// Child environments are UTF-16 blocks; the creation flag selects an input block's encoding.
DWORD snapshotChildEnvironment(const void *block, bool unicode, std::vector<uint16_t> &output);
// Replaces the guest environment without importing host defaults or changing runtime configuration.
DWORD installChildEnvironment(std::span<const uint16_t> block);

GUEST_PTR WINAPI GetCommandLineA();
GUEST_PTR WINAPI GetCommandLineW();
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle);
GUEST_PTR WINAPI GetEnvironmentStrings();
GUEST_PTR WINAPI GetEnvironmentStringsA();
GUEST_PTR WINAPI GetEnvironmentStringsW();
BOOL WINAPI FreeEnvironmentStringsA(LPCH penv);
BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv);
DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize);
DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize);
BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);

} // namespace kernel32
