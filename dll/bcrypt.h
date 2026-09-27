#pragma once

#include "types.h"

using BCRYPT_HANDLE = GUEST_PTR;
using BCRYPT_ALG_HANDLE = BCRYPT_HANDLE;
using BCRYPT_HASH_HANDLE = BCRYPT_HANDLE;
static_assert(sizeof(BCRYPT_ALG_HANDLE) == sizeof(GUEST_PTR));

namespace bcrypt {

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
											ULONG dwFlags);
NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
NTSTATUS WINAPI BCryptGetProperty(BCRYPT_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput, ULONG cbOutput,
								  ULONG *pcbResult, ULONG dwFlags);
NTSTATUS WINAPI BCryptCreateHash(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_HASH_HANDLE *phHash, PUCHAR pbHashObject,
								 ULONG cbHashObject, PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags);
NTSTATUS WINAPI BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash);
NTSTATUS WINAPI BCryptHashData(BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
NTSTATUS WINAPI BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG dwFlags);
NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags);
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);

} // namespace bcrypt
