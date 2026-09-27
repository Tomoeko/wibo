#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bcrypt.h>
#include <ntstatus.h>

#include "test_assert.h"
#include <stddef.h>

typedef NTSTATUS(WINAPI *OpenProviderFn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *CloseProviderFn)(BCRYPT_ALG_HANDLE, ULONG);

struct GuardedHandle {
	ULONG_PTR before[2];
	BCRYPT_ALG_HANDLE handle;
	ULONG_PTR after[2];
};

static OpenProviderFn open_provider;
static CloseProviderFn close_provider;

_Static_assert(sizeof(BCRYPT_ALG_HANDLE) == sizeof(void *), "Algorithm handles have pointer width");
_Static_assert(offsetof(struct GuardedHandle, handle) == 2 * sizeof(ULONG_PTR), "Handle output is aligned");

static NTSTATUS open_observed(const char *label, LPCWSTR algorithm, LPCWSTR implementation, ULONG flags,
							  BCRYPT_ALG_HANDLE *handle) {
	struct GuardedHandle output;
	memset(&output, 0xa5, sizeof(output));
	output.handle = NULL;
	SetLastError(0x4321);
	NTSTATUS status = open_provider(&output.handle, algorithm, implementation, flags);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0x4321, error);
	const BYTE *bytes = (const BYTE *)&output;
	for (unsigned index = 0; index < sizeof(output); ++index)
		if (index < offsetof(struct GuardedHandle, handle) ||
			index >= offsetof(struct GuardedHandle, handle) + sizeof(output.handle))
			TEST_CHECK_EQ(0xa5, bytes[index]);
	if (status < 0)
		TEST_CHECK(output.handle == NULL);
	printf("%s: status=%08lx error=%lu handle-present=%u\n", label, (unsigned long)status, (unsigned long)error,
		   output.handle != NULL);
	if (handle)
		*handle = output.handle;
	return status;
}

static NTSTATUS close_observed(const char *label, BCRYPT_ALG_HANDLE handle) {
	SetLastError(0x4321);
	NTSTATUS status = close_provider(handle, 0);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0x4321, error);
	printf("%s: status=%08lx error=%lu\n", label, (unsigned long)status, (unsigned long)error);
	return status;
}

static void provider_pair(const char *label, LPCWSTR algorithm) {
	BCRYPT_ALG_HANDLE first, second;
	TEST_CHECK_EQ(STATUS_SUCCESS, open_observed(label, algorithm, NULL, 0, &first));
	TEST_CHECK(first != NULL);
	TEST_CHECK_EQ(STATUS_SUCCESS, open_observed(label, algorithm, MS_PRIMITIVE_PROVIDER, 0, &second));
	TEST_CHECK(second != NULL);
	TEST_CHECK(first != second);
	TEST_CHECK_EQ(STATUS_SUCCESS, close_observed("close-first", first));
	TEST_CHECK_EQ(STATUS_SUCCESS, close_observed("close-second", second));
}

int main(void) {
	HMODULE module = LoadLibraryW(L"bcrypt.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "BCryptOpenAlgorithmProvider");
	_Static_assert(sizeof(exported) == sizeof(open_provider), "Open function pointer width");
	memcpy(&open_provider, &exported, sizeof(open_provider));
	TEST_CHECK(open_provider != NULL);
	exported = GetProcAddress(module, "BCryptCloseAlgorithmProvider");
	_Static_assert(sizeof(exported) == sizeof(close_provider), "Close function pointer width");
	memcpy(&close_provider, &exported, sizeof(close_provider));
	TEST_CHECK(close_provider != NULL);

	provider_pair("md5", BCRYPT_MD5_ALGORITHM);
	provider_pair("sha1", BCRYPT_SHA1_ALGORITHM);
	if (getenv("WIBO_EXPECT_BCRYPT_LIMITS")) {
		TEST_CHECK_EQ(STATUS_NOT_SUPPORTED, open_observed("sha256-default", BCRYPT_SHA256_ALGORITHM, NULL, 0, NULL));
		TEST_CHECK_EQ(STATUS_NOT_SUPPORTED,
					  open_observed("sha256-explicit", BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0, NULL));
	} else {
		provider_pair("sha256", BCRYPT_SHA256_ALGORITHM);
	}
	TEST_CHECK_EQ(STATUS_INVALID_PARAMETER, open_observed("null-id", NULL, NULL, 0, NULL));
	TEST_CHECK_EQ(STATUS_NOT_IMPLEMENTED,
				  open_observed("unknown-algorithm", L"FixtureUnknownAlgorithm", NULL, 0, NULL));
	TEST_CHECK_EQ(STATUS_NOT_IMPLEMENTED,
				  open_observed("unknown-provider", BCRYPT_MD5_ALGORITHM, L"FixtureUnknownProvider", 0, NULL));
	TEST_CHECK_EQ(STATUS_NOT_IMPLEMENTED, open_observed("invalid-flags", BCRYPT_MD5_ALGORITHM, NULL, 0x80000000, NULL));
	SetLastError(0x4321);
	NTSTATUS status = open_provider(NULL, BCRYPT_MD5_ALGORITHM, NULL, 0);
	DWORD error = GetLastError();
	printf("null-output: status=%08lx error=%lu\n", (unsigned long)status, (unsigned long)error);
	TEST_CHECK_EQ(STATUS_INVALID_PARAMETER, status);
	TEST_CHECK_EQ(0x4321, error);
	TEST_CHECK_EQ(STATUS_INVALID_HANDLE, close_observed("null-close", NULL));
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
