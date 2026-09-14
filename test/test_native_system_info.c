#define _WIN32_WINNT 0x0501
#include <windows.h>

#include <stdint.h>

#include "test_assert.h"

// https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getnativesysteminfo
// The reference Wine32 process can be WOW64. Wibo currently presents each
// guest as native x86 or x64, so expectations follow the reported process mode.
struct GuardedInfo {
	unsigned char before[16];
	SYSTEM_INFO info;
	unsigned char after[16];
};

static void query_info(struct GuardedInfo *guarded, BOOL native) {
	memset(guarded, 0xa5, sizeof(*guarded));
	SetLastError(0x13572468);
	if (native)
		GetNativeSystemInfo(&guarded->info);
	else
		GetSystemInfo(&guarded->info);
	TEST_CHECK_EQ(0x13572468, GetLastError());
	for (unsigned i = 0; i < 16; ++i) {
		TEST_CHECK_EQ(0xa5, guarded->before[i]);
		TEST_CHECK_EQ(0xa5, guarded->after[i]);
	}
	const SYSTEM_INFO *info = &guarded->info;
	TEST_CHECK_EQ(0, info->wReserved);
	TEST_CHECK(info->dwPageSize != 0 && (info->dwPageSize & (info->dwPageSize - 1)) == 0);
	TEST_CHECK(info->dwAllocationGranularity >= info->dwPageSize);
	TEST_CHECK_EQ(0, info->dwAllocationGranularity % info->dwPageSize);
	TEST_CHECK((uintptr_t)info->lpMinimumApplicationAddress > 0);
	TEST_CHECK((uintptr_t)info->lpMaximumApplicationAddress > (uintptr_t)info->lpMinimumApplicationAddress);
	TEST_CHECK_EQ(0, (uintptr_t)info->lpMinimumApplicationAddress % info->dwPageSize);
	TEST_CHECK_EQ(info->dwPageSize - 1, (uintptr_t)info->lpMaximumApplicationAddress % info->dwPageSize);
	TEST_CHECK(info->dwNumberOfProcessors > 0);
	TEST_CHECK(info->dwActiveProcessorMask != 0);
	unsigned active = 0;
	for (DWORD_PTR mask = info->dwActiveProcessorMask; mask; mask &= mask - 1)
		++active;
	TEST_CHECK(active <= info->dwNumberOfProcessors);
	printf("%s arch=%u type=%lu processors=%lu mask=0x%llx page=%lu granularity=%lu min=0x%llx max=0x%llx level=%u "
		   "revision=%u\n",
		   native ? "native" : "process", info->wProcessorArchitecture, info->dwProcessorType,
		   info->dwNumberOfProcessors, (unsigned long long)info->dwActiveProcessorMask, info->dwPageSize,
		   info->dwAllocationGranularity, (unsigned long long)(uintptr_t)info->lpMinimumApplicationAddress,
		   (unsigned long long)(uintptr_t)info->lpMaximumApplicationAddress, info->wProcessorLevel,
		   info->wProcessorRevision);
}

static void check_equal(const SYSTEM_INFO *a, const SYSTEM_INFO *b) {
	TEST_CHECK_EQ(a->wProcessorArchitecture, b->wProcessorArchitecture);
	TEST_CHECK_EQ(a->wReserved, b->wReserved);
	TEST_CHECK_EQ(a->dwPageSize, b->dwPageSize);
	TEST_CHECK(a->lpMinimumApplicationAddress == b->lpMinimumApplicationAddress);
	TEST_CHECK(a->lpMaximumApplicationAddress == b->lpMaximumApplicationAddress);
	TEST_CHECK(a->dwActiveProcessorMask == b->dwActiveProcessorMask);
	TEST_CHECK_EQ(a->dwNumberOfProcessors, b->dwNumberOfProcessors);
	TEST_CHECK_EQ(a->dwProcessorType, b->dwProcessorType);
	TEST_CHECK_EQ(a->dwAllocationGranularity, b->dwAllocationGranularity);
	TEST_CHECK_EQ(a->wProcessorLevel, b->wProcessorLevel);
	TEST_CHECK_EQ(a->wProcessorRevision, b->wProcessorRevision);
}

int main(void) {
	BOOL wow64 = FALSE;
	TEST_CHECK(IsWow64Process(GetCurrentProcess(), &wow64));
	printf("pointer_bits=%u wow64=%d\n", (unsigned)(sizeof(void *) * 8), wow64);
	struct GuardedInfo process, native, repeat;
	query_info(&process, FALSE);
	query_info(&native, TRUE);
	query_info(&repeat, TRUE);
	check_equal(&native.info, &repeat.info);
#ifdef _WIN64
	TEST_CHECK_EQ(PROCESSOR_ARCHITECTURE_AMD64, process.info.wProcessorArchitecture);
	TEST_CHECK_EQ(PROCESSOR_AMD_X8664, process.info.dwProcessorType);
	TEST_CHECK((uintptr_t)process.info.lpMaximumApplicationAddress > UINT32_MAX);
#else
	TEST_CHECK_EQ(PROCESSOR_ARCHITECTURE_INTEL, process.info.wProcessorArchitecture);
	TEST_CHECK_EQ(PROCESSOR_INTEL_PENTIUM, process.info.dwProcessorType);
#endif
	if (wow64) {
		// This fixture is built for x86/x64 and the reference has x64 emulation.
		TEST_CHECK_EQ(PROCESSOR_ARCHITECTURE_AMD64, native.info.wProcessorArchitecture);
		TEST_CHECK_EQ(PROCESSOR_AMD_X8664, native.info.dwProcessorType);
		TEST_CHECK_EQ(process.info.dwPageSize, native.info.dwPageSize);
		TEST_CHECK_EQ(process.info.dwAllocationGranularity, native.info.dwAllocationGranularity);
	} else {
		check_equal(&process.info, &native.info);
	}

	// Validate the reported geometry against a small allocation owned here.
	void *owned = VirtualAlloc(NULL, process.info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(owned != NULL);
	TEST_CHECK((uintptr_t)owned >= (uintptr_t)process.info.lpMinimumApplicationAddress);
	TEST_CHECK((uintptr_t)owned <= (uintptr_t)process.info.lpMaximumApplicationAddress);
	TEST_CHECK_EQ(0, (uintptr_t)owned % process.info.dwAllocationGranularity);
	MEMORY_BASIC_INFORMATION memory;
	TEST_CHECK_EQ(sizeof(memory), VirtualQuery(owned, &memory, sizeof(memory)));
	TEST_CHECK_EQ(MEM_COMMIT, memory.State);
	TEST_CHECK_EQ(PAGE_READWRITE, memory.Protect);
	TEST_CHECK(memory.RegionSize >= process.info.dwPageSize);
	TEST_CHECK_EQ(0, memory.RegionSize % process.info.dwPageSize);
	TEST_CHECK(VirtualFree(owned, 0, MEM_RELEASE));
	puts("native system info tests passed: architecture, geometry, processor fields, canaries, repeat, last error");
	return 0;
}
