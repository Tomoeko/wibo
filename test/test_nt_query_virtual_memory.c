#include <windows.h>
#include <winternl.h>

#include <stdint.h>
#include <string.h>

#include "test_assert.h"

typedef NTSTATUS(WINAPI *QueryMemory)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);

static QueryMemory query;
static const DWORD seed = 0x13579bdf;
static const NTSTATUS success = 0;
static const NTSTATUS lengthMismatch = (NTSTATUS)0xc0000004;
static const NTSTATUS invalidClass = (NTSTATUS)0xc0000003;
static const NTSTATUS invalidHandle = (NTSTATUS)0xc0000008;
static const NTSTATUS typeMismatch = (NTSTATUS)0xc0000024;
static const NTSTATUS accessDenied = (NTSTATUS)0xc0000022;
static const NTSTATUS invalidParameter = (NTSTATUS)0xc000000d;
static const NTSTATUS notSupported = (NTSTATUS)0xc00000bb;

union BasicPacket {
	MEMORY_BASIC_INFORMATION information;
	BYTE bytes[sizeof(MEMORY_BASIC_INFORMATION) + 16];
};

static MEMORY_BASIC_INFORMATION check_query(HANDLE process, PVOID address, ULONG kind, SIZE_T length,
											NTSTATUS expectedStatus, BOOL nullBuffer, BOOL nullLength) {
	union BasicPacket packet;
	memset(&packet, 0xa5, sizeof(packet));
	struct {
		SIZE_T before;
		SIZE_T value;
		SIZE_T after;
	} returned = {seed, seed, seed};
	SetLastError(seed);
	TEST_CHECK_EQ(expectedStatus, query(process, address, kind, nullBuffer ? NULL : &packet, length,
										nullLength ? NULL : &returned.value));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(seed, returned.before);
	TEST_CHECK_EQ(seed, returned.after);
	SIZE_T expectedLength = expectedStatus == success ? sizeof(MEMORY_BASIC_INFORMATION) : seed;
#ifndef _WIN64
	if (kind == 0 && expectedStatus == lengthMismatch)
		expectedLength = sizeof(MEMORY_BASIC_INFORMATION);
#endif
	TEST_CHECK_EQ(nullLength ? seed : expectedLength, returned.value);
	for (size_t index = expectedStatus == success ? sizeof(MEMORY_BASIC_INFORMATION) : 0; index < sizeof(packet);
		 ++index)
		TEST_CHECK_EQ(0xa5, packet.bytes[index]);
#ifdef _WIN64
	if (expectedStatus == success) {
		for (size_t index = 20; index < 24; ++index)
			TEST_CHECK_EQ(0xa5, packet.bytes[index]);
		for (size_t index = 44; index < 48; ++index)
			TEST_CHECK_EQ(0xa5, packet.bytes[index]);
	}
#endif
	return packet.information;
}

static void check_region(PVOID address, PVOID base, PVOID allocation, SIZE_T size, DWORD state, DWORD protection,
						 DWORD allocationProtection, DWORD type) {
	MEMORY_BASIC_INFORMATION information =
		check_query(GetCurrentProcess(), address, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, FALSE);
	TEST_CHECK(information.BaseAddress == base);
	TEST_CHECK(information.AllocationBase == allocation);
	TEST_CHECK_U64_EQ(size, information.RegionSize);
	TEST_CHECK_EQ(state, information.State);
	TEST_CHECK_EQ(protection, information.Protect);
	TEST_CHECK_EQ(allocationProtection, information.AllocationProtect);
	TEST_CHECK_EQ(type, information.Type);

	MEMORY_BASIC_INFORMATION ordinary;
	TEST_CHECK_EQ(sizeof(ordinary), VirtualQuery(address, &ordinary, sizeof(ordinary)));
	TEST_CHECK(ordinary.BaseAddress == base);
	TEST_CHECK_U64_EQ(size, ordinary.RegionSize);
}

int main(int argc, char **argv) {
	FARPROC entry = GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryVirtualMemory");
	TEST_CHECK(entry != NULL);
	memcpy(&query, &entry, sizeof(query));
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	const SIZE_T page = system.dwPageSize;
	BYTE *memory = VirtualAlloc(NULL, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(memory != NULL);
	HANDLE current = GetCurrentProcess();
	const SIZE_T lengths[] = {0, 1, sizeof(MEMORY_BASIC_INFORMATION) - 1, sizeof(MEMORY_BASIC_INFORMATION),
							  sizeof(MEMORY_BASIC_INFORMATION) + 1};
	for (unsigned index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index)
		check_query(current, memory, 0, lengths[index],
					lengths[index] < sizeof(MEMORY_BASIC_INFORMATION) ? lengthMismatch : success, FALSE, FALSE);
	check_query(current, memory, 0, 0, lengthMismatch, TRUE, FALSE);
	check_query(current, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, TRUE);
	check_region(memory + page + 3, memory + page, memory, page * 3, MEM_COMMIT, PAGE_READWRITE, PAGE_READWRITE,
				 MEM_PRIVATE);
	DWORD oldProtection = 0;
	TEST_CHECK(VirtualProtect(memory + page * 2, page, PAGE_READONLY, &oldProtection));
	check_region(memory + page + 3, memory + page, memory, page, MEM_COMMIT, PAGE_READWRITE, PAGE_READWRITE,
				 MEM_PRIVATE);
	check_region(memory + page * 2 + 3, memory + page * 2, memory, page, MEM_COMMIT, PAGE_READONLY, PAGE_READWRITE,
				 MEM_PRIVATE);
	TEST_CHECK(VirtualProtect(memory + page * 3, page, PAGE_READWRITE | PAGE_GUARD, &oldProtection));
	check_region(memory + page * 3 + 3, memory + page * 3, memory, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD,
				 PAGE_READWRITE, MEM_PRIVATE);
	TEST_CHECK(VirtualFree(memory + page * 2, page, MEM_DECOMMIT));
	check_region(memory + page * 2 + 3, memory + page * 2, memory, page, MEM_RESERVE, 0, PAGE_READWRITE, MEM_PRIVATE);

	MEMORY_BASIC_INFORMATION freeRegion =
		check_query(current, NULL, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, FALSE);
	TEST_CHECK(freeRegion.BaseAddress == NULL);
	TEST_CHECK(freeRegion.AllocationBase == NULL);
	TEST_CHECK(freeRegion.RegionSize >= page);
	TEST_CHECK_EQ(0, freeRegion.AllocationProtect);
	TEST_CHECK_EQ(MEM_FREE, freeRegion.State);
	TEST_CHECK_EQ(PAGE_NOACCESS, freeRegion.Protect);
	TEST_CHECK_EQ(0, freeRegion.Type);
	check_query(current, (PVOID)(ULONG_PTR)-1, 0, sizeof(MEMORY_BASIC_INFORMATION), invalidParameter, FALSE, FALSE);
	check_query(NULL, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), invalidHandle, FALSE, FALSE);
	check_query(NULL, memory, 0, 1, lengthMismatch, FALSE, FALSE);
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	check_query(event, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), typeMismatch, FALSE, FALSE);
	check_query(event, memory, 0, 1, lengthMismatch, FALSE, FALSE);
	TEST_CHECK(CloseHandle(event));

	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(process != NULL);
	check_query(process, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, FALSE);
	HANDLE reduced = NULL;
	TEST_CHECK(DuplicateHandle(current, process, current, &reduced, 0, FALSE, 0));
	check_query(reduced, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), accessDenied, FALSE, FALSE);
	check_query(reduced, memory, 0, 1, lengthMismatch, FALSE, FALSE);
	TEST_CHECK(CloseHandle(reduced));
	TEST_CHECK(CloseHandle(process));
	process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(process != NULL);
	check_query(process, memory, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, FALSE);
	TEST_CHECK(CloseHandle(process));

	PVOID image = GetModuleHandleA(NULL);
	MEMORY_BASIC_INFORMATION imageRegion =
		check_query(current, image, 0, sizeof(MEMORY_BASIC_INFORMATION), success, FALSE, FALSE);
	TEST_CHECK(imageRegion.BaseAddress == image);
	TEST_CHECK(imageRegion.AllocationBase == image);
	TEST_CHECK_EQ(MEM_COMMIT, imageRegion.State);
	TEST_CHECK_EQ(MEM_IMAGE, imageRegion.Type);
	TEST_CHECK(imageRegion.RegionSize >= page);
	TEST_CHECK(imageRegion.Protect != 0);

	check_query(current, memory, 0x7fffffff, 0, invalidClass, FALSE, FALSE);
	check_query(NULL, memory, 0x7fffffff, sizeof(MEMORY_BASIC_INFORMATION), invalidClass, FALSE, FALSE);
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)(page * 3), NULL);
	TEST_CHECK(mapping != NULL);
	BYTE *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, page * 3);
	TEST_CHECK(view != NULL);
	check_region(view + page + 3, view + page, view, page * 2, MEM_COMMIT, PAGE_READWRITE, PAGE_READWRITE, MEM_MAPPED);
	TEST_CHECK(UnmapViewOfFile(view));
	TEST_CHECK(CloseHandle(mapping));

	if (argc == 2 && strcmp(argv[1], "--unsupported") == 0) {
		const ULONG classes[] = {2, 3, 4, 6, 1000, 1001};
		for (unsigned index = 0; index < sizeof(classes) / sizeof(classes[0]); ++index) {
			check_query(current, image, classes[index], 0, notSupported, FALSE, FALSE);
			check_query(current, image, classes[index], sizeof(MEMORY_BASIC_INFORMATION), notSupported, FALSE, FALSE);
		}
	}
	TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));
	return 0;
}
