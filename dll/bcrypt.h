#pragma once

#include "types.h"

using BCRYPT_ALG_HANDLE = GUEST_PTR;
static_assert(sizeof(BCRYPT_ALG_HANDLE) == sizeof(GUEST_PTR));

namespace bcrypt {

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
											ULONG dwFlags);
NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags);
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);

} // namespace bcrypt
