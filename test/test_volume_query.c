#include "test_assert.h"
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>
typedef NTSTATUS(WINAPI *Query)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
typedef struct {
	LONGLONG total, available;
	ULONG sectors, bytes;
} Size;
typedef struct {
	LONGLONG total, caller, actual;
	ULONG sectors, bytes;
} Full;
static Query query;
static void success(HANDLE file, ULONG kind, void *data, ULONG length, ULONG used) {
	IO_STATUS_BLOCK block;
	memset(&block, 0x71, sizeof(block));
	SetLastError(0x51);
	TEST_CHECK_EQ(0, query(file, &block, data, length, kind));
	TEST_CHECK_EQ(0, block.Status);
	TEST_CHECK_EQ(used, block.Information);
	TEST_CHECK_EQ(0x51, GetLastError());
}
int main(void) {
	query = (Query)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryVolumeInformationFile");
	TEST_CHECK(query != NULL);
	HANDLE file = CreateFileA("wibo_volume_fixture.tmp", GENERIC_READ | GENERIC_WRITE,
							  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
							  FILE_FLAG_DELETE_ON_CLOSE, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	Size size;
	success(file, 3, &size, sizeof(size), sizeof(size));
	TEST_CHECK(size.total > 0 && size.available >= 0 && size.available <= size.total);
	TEST_CHECK(size.sectors > 0 && size.bytes > 0);
	Full full;
	success(file, 7, &full, sizeof(full), sizeof(full));
	TEST_CHECK_EQ(size.total, full.total);
	TEST_CHECK_EQ(size.sectors, full.sectors);
	TEST_CHECK_EQ(size.bytes, full.bytes);
	TEST_CHECK(full.caller >= 0 && full.caller <= full.total && full.actual >= full.caller &&
			   full.actual <= full.total);
	ULONG device[2];
	success(file, 4, device, sizeof(device), sizeof(device));
	TEST_CHECK(device[0] != 0);
	BYTE data[512];
	IO_STATUS_BLOCK block;
	memset(data, 0x61, sizeof(data));
	SetLastError(0x51);
	TEST_CHECK_EQ(0, query(file, &block, data, sizeof(data), 5));
	TEST_CHECK_EQ(0, block.Status);
	TEST_CHECK(block.Information >= 12 && block.Information <= sizeof(data));
	TEST_CHECK_EQ(0x51, GetLastError());
	ULONG header[3];
	memcpy(header, data, sizeof(header));
	TEST_CHECK(header[1] > 0 && header[2] % 2 == 0);
	TEST_CHECK_EQ(12 + header[2], block.Information);
	for (unsigned i = (unsigned)block.Information; i < sizeof(data); ++i)
		TEST_CHECK_EQ(0x61, data[i]);
	memset(data, 0x61, sizeof(data));
	TEST_CHECK_U64_EQ(0xc0000023U, (ULONG)query(file, &block, data, 23, 3));
	TEST_CHECK_U64_EQ(0xc0000023U, (ULONG)block.Status);
	TEST_CHECK_EQ(0, block.Information);
	for (unsigned i = 0; i < sizeof(data); ++i)
		TEST_CHECK_EQ(0x61, data[i]);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(4096, size.total);
		TEST_CHECK_EQ(1024, size.available);
		const char *faults[] = {"truncated", "trailing", "oversized", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_VOLUME_RESPONSE", faults[i]));
			memset(data, 0x61, sizeof(data));
			TEST_CHECK_U64_EQ(i == 3 ? 0xc0000022U : 0xc00000e9U, (ULONG)query(file, &block, data, sizeof(data), 3));
			for (unsigned j = 0; j < sizeof(data); ++j)
				TEST_CHECK_EQ(0x61, data[j]);
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_VOLUME_RESPONSE", NULL));
	}
	TEST_CHECK(CloseHandle(file));
	return 0;
}
