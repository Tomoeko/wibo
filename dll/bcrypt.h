#pragma once

#include "types.h"

using BCRYPT_HANDLE = GUEST_PTR;
using BCRYPT_ALG_HANDLE = BCRYPT_HANDLE;
static_assert(sizeof(BCRYPT_ALG_HANDLE) == sizeof(GUEST_PTR));

namespace bcrypt {

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
											ULONG dwFlags);
NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
NTSTATUS WINAPI BCryptGetProperty(BCRYPT_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput, ULONG cbOutput,
								  ULONG *pcbResult, ULONG dwFlags);
NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags);
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);

} // namespace bcrypt
