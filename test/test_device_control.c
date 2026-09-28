#include "test_assert.h"
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(WINAPI *Control)(HANDLE, HANDLE, void *, void *, PIO_STATUS_BLOCK, ULONG, void *, ULONG, void *,
								  ULONG);
static Control control;
static HANDLE event;
static void failed(HANDLE file, ULONG code, ULONG length, ULONG expected) {
	IO_STATUS_BLOCK block;
	memset(&block, 0x71, sizeof(block));
	IO_STATUS_BLOCK saved = block;
	BYTE output[64];
	memset(output, 0x61, sizeof(output));
	TEST_CHECK(ResetEvent(event));
	SetLastError(0x71);
	TEST_CHECK_U64_EQ(expected, (ULONG)control(file, event, NULL, NULL, &block, code, NULL, 0, output, length));
	TEST_CHECK(memcmp(&block, &saved, sizeof(block)) == 0);
	for (unsigned i = 0; i < sizeof(output); ++i)
		TEST_CHECK_EQ(0x61, output[i]);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(event, 0));
	TEST_CHECK_EQ(0x71, GetLastError());
}
static void peek(HANDLE pipe, ULONG capacity, ULONG state, ULONG available, ULONG copied) {
	BYTE output[64];
	memset(output, 0x61, sizeof(output));
	IO_STATUS_BLOCK block;
	TEST_CHECK(ResetEvent(event));
	SetLastError(0x71);
	TEST_CHECK_EQ(0, control(pipe, event, NULL, NULL, &block, 0x11400c, NULL, 0, output, capacity));
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK_EQ(0, block.Status);
	TEST_CHECK_EQ(16 + copied, block.Information);
	ULONG fields[4];
	memcpy(fields, output, sizeof(fields));
	TEST_CHECK_EQ(state, fields[0]);
	TEST_CHECK_EQ(available, fields[1]);
	TEST_CHECK_EQ(0, fields[2]);
	TEST_CHECK_EQ(0, fields[3]);
	TEST_CHECK(memcmp(output + 16, "abc", copied) == 0);
	for (unsigned i = 16 + copied; i < sizeof(output); ++i)
		TEST_CHECK_EQ(0x61, output[i]);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 0));
}
int main(void) {
	HMODULE module = LoadLibraryA("ntdll.dll");
	TEST_CHECK(module != NULL);
	control = (Control)(void *)GetProcAddress(module, "NtDeviceIoControlFile");
	TEST_CHECK(control != NULL);
	event = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	HANDLE file = CreateFileA("wibo_device_fixture.tmp", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
							  FILE_FLAG_DELETE_ON_CLOSE, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	failed(file, 0x222000, 64, 0xc00000bbU);
	failed(INVALID_HANDLE_VALUE, 0x222000, 64, 0xc0000008U);
	TEST_CHECK(CloseHandle(file));
	HANDLE pipe = CreateNamedPipeA("\\\\.\\pipe\\wibo_device_fixture", PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
								   1, 1024, 1024, 0, NULL);
	TEST_CHECK(pipe != INVALID_HANDLE_VALUE);
	failed(pipe, 0x11400c, 64, 0xc00000adU);
	HANDLE client =
		CreateFileA("\\\\.\\pipe\\wibo_device_fixture", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	TEST_CHECK(client != INVALID_HANDLE_VALUE);
	DWORD count;
	TEST_CHECK(WriteFile(client, "abc", 3, &count, NULL));
	TEST_CHECK_EQ(3, count);
	BYTE directOutput[64] = {0};
	DWORD directBytes = 0;
	TEST_CHECK(DeviceIoControl(pipe, 0x11400c, NULL, 0, directOutput, sizeof(directOutput), &directBytes, NULL));
	TEST_CHECK_EQ(19, directBytes);
	ULONG directAvailable = 0;
	memcpy(&directAvailable, directOutput + sizeof(ULONG), sizeof(directAvailable));
	TEST_CHECK_EQ(3, directAvailable);
	TEST_CHECK(memcmp(directOutput + 16, "abc", 3) == 0);
	failed(pipe, 0x11400c, 15, 0xc0000004U);
	for (ULONG capacity = 16; capacity <= 20; ++capacity)
		peek(pipe, capacity, 3, 3, capacity < 19 ? capacity - 16 : 3);
	TEST_CHECK(CloseHandle(client));
	peek(pipe, 64, 4, 3, 3);
	BYTE bytes[3];
	TEST_CHECK(ReadFile(pipe, bytes, 3, &count, NULL));
	TEST_CHECK_EQ(3, count);
	failed(pipe, 0x11400c, 64, 0xc000014bU);
	TEST_CHECK(CloseHandle(pipe));
	TEST_CHECK(CloseHandle(event));
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
