#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bcrypt.h>
#include <ntstatus.h>

#include "test_assert.h"

typedef NTSTATUS(WINAPI *OpenProviderFn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *CloseProviderFn)(BCRYPT_ALG_HANDLE, ULONG);
typedef NTSTATUS(WINAPI *GetPropertyFn)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);

enum { OUTPUT_CAPACITY = 16 };

struct GuardedOutput {
	ULONG before[4];
	BYTE data[OUTPUT_CAPACITY];
	ULONG after[4];
};

static OpenProviderFn open_provider;
static CloseProviderFn close_provider;
static GetPropertyFn get_property;

static NTSTATUS property_call(const char *label, BCRYPT_ALG_HANDLE algorithm, LPCWSTR property,
							  struct GuardedOutput *output, ULONG capacity, ULONG *returned) {
	TEST_CHECK(capacity <= OUTPUT_CAPACITY);
	SetLastError(0x4321);
	NTSTATUS status = get_property(algorithm, property, output ? output->data : NULL, capacity, returned, 0);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0x4321, error);
	if (output) {
		for (unsigned index = 0; index < sizeof(output->before); ++index)
			TEST_CHECK_EQ(0xa5, ((BYTE *)output->before)[index]);
		for (unsigned index = capacity; index < sizeof(output->data); ++index)
			TEST_CHECK_EQ(0xa5, output->data[index]);
		for (unsigned index = 0; index < sizeof(output->after); ++index)
			TEST_CHECK_EQ(0xa5, ((BYTE *)output->after)[index]);
		if (status < 0)
			for (unsigned index = 0; index < sizeof(output->data); ++index)
				TEST_CHECK_EQ(0xa5, output->data[index]);
	}
	printf("%s: capacity=%lu status=%08lx error=%lu result-present=%u result=%lu\n", label, (unsigned long)capacity,
		   (unsigned long)status, (unsigned long)error, returned != NULL, returned ? (unsigned long)*returned : 0);
	return status;
}

static void check_hash_length(const char *label, LPCWSTR name, ULONG expected) {
	BCRYPT_ALG_HANDLE algorithm = NULL;
	SetLastError(0x4321);
	TEST_CHECK_EQ(STATUS_SUCCESS, open_provider(&algorithm, name, NULL, 0));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(algorithm != NULL);
	ULONG returned = 0xffffffff;
	TEST_CHECK_EQ(STATUS_BUFFER_TOO_SMALL,
				  property_call("query-zero", algorithm, BCRYPT_HASH_LENGTH, NULL, 0, &returned));
	TEST_CHECK_EQ(sizeof(ULONG), returned);
	returned = 0xffffffff;
	TEST_CHECK_EQ(STATUS_SUCCESS, property_call("query-sized", algorithm, BCRYPT_HASH_LENGTH, NULL, 4, &returned));
	TEST_CHECK_EQ(sizeof(ULONG), returned);
	struct GuardedOutput output;
	memset(&output, 0xa5, sizeof(output));
	returned = 0xffffffff;
	TEST_CHECK_EQ(STATUS_BUFFER_TOO_SMALL,
				  property_call("short", algorithm, BCRYPT_HASH_LENGTH, &output, 3, &returned));
	TEST_CHECK_EQ(sizeof(ULONG), returned);
	for (unsigned pass = 0; pass < 2; ++pass) {
		ULONG capacity = pass ? OUTPUT_CAPACITY : sizeof(ULONG);
		memset(&output, 0xa5, sizeof(output));
		returned = 0xffffffff;
		TEST_CHECK_EQ(STATUS_SUCCESS,
					  property_call(label, algorithm, BCRYPT_HASH_LENGTH, &output, capacity, &returned));
		TEST_CHECK_EQ(sizeof(ULONG), returned);
		ULONG value;
		memcpy(&value, output.data, sizeof(value));
		TEST_CHECK_EQ(expected, value);
		for (unsigned index = sizeof(value); index < sizeof(output.data); ++index)
			TEST_CHECK_EQ(0xa5, output.data[index]);
	}
	memset(&output, 0xa5, sizeof(output));
	returned = 0xffffffff;
	TEST_CHECK_EQ(STATUS_INVALID_PARAMETER, property_call("null-property", algorithm, NULL, &output, 4, &returned));
	TEST_CHECK_EQ(0xffffffff, returned);
	TEST_CHECK_EQ(STATUS_INVALID_PARAMETER,
				  property_call("null-result", algorithm, BCRYPT_HASH_LENGTH, &output, 4, NULL));
	SetLastError(0x4321);
	TEST_CHECK_EQ(STATUS_SUCCESS, close_provider(algorithm, 0));
	TEST_CHECK_EQ(0x4321, GetLastError());
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
	exported = GetProcAddress(module, "BCryptGetProperty");
	_Static_assert(sizeof(exported) == sizeof(get_property), "Property function pointer width");
	memcpy(&get_property, &exported, sizeof(get_property));
	TEST_CHECK(get_property != NULL);
	check_hash_length("md5-length", BCRYPT_MD5_ALGORITHM, 16);
	check_hash_length("sha1-length", BCRYPT_SHA1_ALGORITHM, 20);
	if (!getenv("WIBO_EXPECT_BCRYPT_LIMITS"))
		check_hash_length("sha256-length", BCRYPT_SHA256_ALGORITHM, 32);
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
