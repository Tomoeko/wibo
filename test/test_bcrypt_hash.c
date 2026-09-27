#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bcrypt.h>
#include <ntstatus.h>

#include "test_assert.h"

typedef NTSTATUS(WINAPI *OpenFn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *CloseFn)(BCRYPT_ALG_HANDLE, ULONG);
typedef NTSTATUS(WINAPI *CreateFn)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *UpdateFn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *FinishFn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *DestroyFn)(BCRYPT_HASH_HANDLE);

static OpenFn open_algorithm;
static CloseFn close_algorithm;
static CreateFn create_hash;
static UpdateFn update_hash;
static FinishFn finish_hash;
static DestroyFn destroy_hash;

#define CHECK_SUCCESS(call)                                                                                            \
	do {                                                                                                               \
		SetLastError(0x4321);                                                                                          \
		NTSTATUS status = (call);                                                                                      \
		DWORD error = GetLastError();                                                                                  \
		TEST_CHECK_EQ(STATUS_SUCCESS, status);                                                                         \
		TEST_CHECK_EQ(0x4321, error);                                                                                  \
	} while (0)

struct Algorithm {
	const char *label;
	LPCWSTR name;
	ULONG size;
	const char *empty;
	const char *abc;
	const char *multiple_blocks;
};

static const struct Algorithm algorithms[] = {
	{"md5", BCRYPT_MD5_ALGORITHM, 16, "d41d8cd98f00b204e9800998ecf8427e", "900150983cd24fb0d6963f7d28e17f72",
	 "57edf4a22be3c955ac49da2e2107b67a"},
	{"sha1", BCRYPT_SHA1_ALGORITHM, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709",
	 "a9993e364706816aba3e25717850c26c9cd0d89d", "dea356a2cddd90c7a7ecedc5ebb563934f460452"},
	{"sha256", BCRYPT_SHA256_ALGORITHM, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	 "594847328451bdfa85056225462cc1d867d877fb388df0ce35f25ab5562bfbb5"},
};

static BCRYPT_HASH_HANDLE create(BCRYPT_ALG_HANDLE algorithm, ULONG flags) {
	struct {
		ULONG_PTR before[2];
		BCRYPT_HASH_HANDLE hash;
		ULONG_PTR after[2];
	} output;
	memset(&output, 0xa5, sizeof(output));
	output.hash = NULL;
	CHECK_SUCCESS(create_hash(algorithm, &output.hash, NULL, 0, NULL, 0, flags));
	TEST_CHECK(output.hash != NULL);
	for (unsigned index = 0; index < sizeof(output.before); ++index)
		TEST_CHECK_EQ(0xa5, ((BYTE *)output.before)[index]);
	for (unsigned index = 0; index < sizeof(output.after); ++index)
		TEST_CHECK_EQ(0xa5, ((BYTE *)output.after)[index]);
	return output.hash;
}

static void update(BCRYPT_HASH_HANDLE hash, const BYTE *input, ULONG length) {
	BYTE copy[640];
	TEST_CHECK(length <= sizeof(copy));
	memset(copy, 0xa5, sizeof(copy));
	memcpy(copy, input, length);
	CHECK_SUCCESS(update_hash(hash, copy, length, 0));
	TEST_CHECK(memcmp(copy, input, length) == 0);
	for (unsigned index = length; index < sizeof(copy); ++index)
		TEST_CHECK_EQ(0xa5, copy[index]);
}

static void update_split(BCRYPT_HASH_HANDLE hash, const BYTE *input, ULONG length) {
	ULONG first = length ? 1 : 0;
	ULONG second = length - first;
	if (second > 63)
		second = 63;
	update(hash, input, first);
	update(hash, input + first, second);
	update(hash, input + first + second, length - first - second);
}

static BYTE hex_digit(char value) {
	TEST_CHECK((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
	return (BYTE)(value <= '9' ? value - '0' : value - 'a' + 10);
}

static void finish(BCRYPT_HASH_HANDLE hash, const struct Algorithm *algorithm, const char *expected) {
	BYTE output[16 + 32 + 16];
	memset(output, 0xa5, sizeof(output));
	CHECK_SUCCESS(finish_hash(hash, output + 16, algorithm->size, 0));
	TEST_CHECK_EQ(2 * algorithm->size, strlen(expected));
	for (unsigned index = 0; index < algorithm->size; ++index)
		TEST_CHECK_EQ((hex_digit(expected[2 * index]) << 4) | hex_digit(expected[2 * index + 1]), output[16 + index]);
	for (unsigned index = 0; index < sizeof(output); ++index)
		if (index < 16 || index >= 16 + algorithm->size)
			TEST_CHECK_EQ(0xa5, output[index]);
}

static void bad_finish(BCRYPT_HASH_HANDLE hash, ULONG capacity) {
	BYTE output[16 + 33 + 16];
	memset(output, 0xa5, sizeof(output));
	SetLastError(0x4321);
	NTSTATUS status = finish_hash(hash, output + 16, capacity, 0);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(STATUS_INVALID_PARAMETER, status);
	TEST_CHECK_EQ(0x4321, error);
	for (unsigned index = 0; index < sizeof(output); ++index)
		TEST_CHECK_EQ(0xa5, output[index]);
}

static void observe_finished_hash(BCRYPT_HASH_HANDLE hash, const struct Algorithm *algorithm) {
	BYTE input = '!';
	SetLastError(0x4321);
	NTSTATUS update_status = update_hash(hash, &input, 1, 0);
	DWORD update_error = GetLastError();
	TEST_CHECK_EQ(0x4321, update_error);
	TEST_CHECK_EQ('!', input);
	BYTE output[16 + 32 + 16];
	memset(output, 0xa5, sizeof(output));
	SetLastError(0x4321);
	NTSTATUS finish_status = finish_hash(hash, output + 16, algorithm->size, 0);
	DWORD finish_error = GetLastError();
	TEST_CHECK_EQ(0x4321, finish_error);
	for (unsigned index = 0; index < sizeof(output); ++index)
		if (finish_status < 0 || index < 16 || index >= 16 + algorithm->size)
			TEST_CHECK_EQ(0xa5, output[index]);
	printf("%s-finished: update-status=%08lx finish-status=%08lx errors=%lu,%lu\n", algorithm->label,
		   (unsigned long)update_status, (unsigned long)finish_status, (unsigned long)update_error,
		   (unsigned long)finish_error);
	if (getenv("WIBO_EXPECT_BCRYPT_STATE")) {
		TEST_CHECK_EQ(STATUS_INVALID_HANDLE, update_status);
		TEST_CHECK_EQ(STATUS_INVALID_HANDLE, finish_status);
	}
}

static void run_algorithm(const struct Algorithm *algorithm, unsigned index) {
	BYTE multi[640];
	ULONG multi_length = index == 0 ? 80 : sizeof(multi);
	for (unsigned position = 0; position < multi_length; ++position)
		multi[position] = (BYTE)(index == 0 ? '0' + (position + 1) % 10 : '0' + position % 8);
	const BYTE *messages[] = {(const BYTE *)"", (const BYTE *)"abc", multi};
	const ULONG lengths[] = {0, 3, multi_length};
	const char *expected[] = {algorithm->empty, algorithm->abc, algorithm->multiple_blocks};
	BCRYPT_ALG_HANDLE provider = NULL;
	CHECK_SUCCESS(open_algorithm(&provider, algorithm->name, NULL, 0));
	TEST_CHECK(provider != NULL);
	for (unsigned vector = 0; vector < 3; ++vector)
		for (unsigned split = 0; split < 2; ++split) {
			BCRYPT_HASH_HANDLE hash = create(provider, 0);
			if (split)
				update_split(hash, messages[vector], lengths[vector]);
			else
				update(hash, messages[vector], lengths[vector]);
			finish(hash, algorithm, expected[vector]);
			if (vector == 1 && split == 0)
				observe_finished_hash(hash, algorithm);
			CHECK_SUCCESS(destroy_hash(hash));
		}
	BCRYPT_HASH_HANDLE reusable = create(provider, BCRYPT_HASH_REUSABLE_FLAG);
	for (unsigned vector = 0; vector < 3; ++vector) {
		update_split(reusable, messages[vector], lengths[vector]);
		finish(reusable, algorithm, expected[vector]);
	}
	finish(reusable, algorithm, algorithm->empty);
	CHECK_SUCCESS(destroy_hash(reusable));
	BCRYPT_HASH_HANDLE first = create(provider, 0), second = create(provider, 0);
	TEST_CHECK(first != second);
	update(first, (const BYTE *)"a", 1);
	update_split(second, multi, multi_length);
	CHECK_SUCCESS(close_algorithm(provider, 0));
	bad_finish(first, algorithm->size - 1);
	bad_finish(first, algorithm->size + 1);
	update(first, (const BYTE *)"bc", 2);
	finish(first, algorithm, algorithm->abc);
	finish(second, algorithm, algorithm->multiple_blocks);
	CHECK_SUCCESS(destroy_hash(first));
	CHECK_SUCCESS(destroy_hash(second));
	printf("%s: vectors=3 split-equivalence=3 reusable-finishes=4 independent-hashes=2 bad-sizes=2\n",
		   algorithm->label);
}

#define RESOLVE(variable, name)                                                                                        \
	do {                                                                                                               \
		FARPROC exported = GetProcAddress(module, name);                                                               \
		_Static_assert(sizeof(exported) == sizeof(variable), "Resolved function pointer width");                       \
		memcpy(&(variable), &exported, sizeof(variable));                                                              \
		TEST_CHECK((variable) != NULL);                                                                                \
	} while (0)

int main(void) {
	HMODULE module = LoadLibraryW(L"bcrypt.dll");
	TEST_CHECK(module != NULL);
	RESOLVE(open_algorithm, "BCryptOpenAlgorithmProvider");
	RESOLVE(close_algorithm, "BCryptCloseAlgorithmProvider");
	RESOLVE(create_hash, "BCryptCreateHash");
	RESOLVE(update_hash, "BCryptHashData");
	RESOLVE(finish_hash, "BCryptFinishHash");
	RESOLVE(destroy_hash, "BCryptDestroyHash");
	unsigned algorithm_count = getenv("WIBO_EXPECT_BCRYPT_LIMITS") ? 2 : sizeof(algorithms) / sizeof(algorithms[0]);
	for (unsigned index = 0; index < algorithm_count; ++index)
		run_algorithm(&algorithms[index], index);
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
