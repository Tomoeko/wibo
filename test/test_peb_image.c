#include "fixture_peb_image.h"
#include "test_assert.h"

__declspec(dllimport) const FixturePebImageObservation *__cdecl getPebImageAttachObservation(void);

static FixturePebImageObservation tlsObservation;

static void NTAPI observeImageAtTls(PVOID instance, DWORD reason, PVOID reserved) {
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		observePebImage(&tlsObservation, NULL);
	}
}

PIMAGE_TLS_CALLBACK pebImageTlsCallback __attribute__((section(".CRT$XLB"), used)) = observeImageAtTls;

static void checkObservation(const FixturePebImageObservation *observation, HMODULE mainModule) {
	TEST_CHECK(observation != NULL);
	TEST_CHECK_EQ(1, observation->calls);
	TEST_CHECK(observation->teb != 0);
	TEST_CHECK(observation->peb != 0);
	TEST_CHECK(observation->imageBase != 0);
	TEST_CHECK_U64_EQ((ULONG_PTR)mainModule, observation->mainModule);
	TEST_CHECK_U64_EQ((ULONG_PTR)mainModule, observation->imageBase);
}

int main(void) {
	HMODULE mainModule = GetModuleHandleA(NULL);
	TEST_CHECK(mainModule != NULL);

	const FixturePebImageObservation *attach = getPebImageAttachObservation();
	checkObservation(attach, mainModule);
	TEST_CHECK(attach->dependency != 0);
	TEST_CHECK(attach->dependency != attach->imageBase);
	checkObservation(&tlsObservation, mainModule);

	FixturePebImageObservation current = {0};
	observePebImage(&current, NULL);
	checkObservation(&current, mainModule);
	TEST_CHECK_U64_EQ(attach->peb, current.peb);
	TEST_CHECK_U64_EQ(tlsObservation.peb, current.peb);

	MEMORY_BASIC_INFORMATION region = {0};
	TEST_CHECK_EQ(sizeof(region), VirtualQuery(mainModule, &region, sizeof(region)));
	TEST_CHECK(region.AllocationBase == mainModule);
	TEST_CHECK_EQ(MEM_COMMIT, region.State);
	TEST_CHECK(region.RegionSize >= sizeof(IMAGE_DOS_HEADER));
	TEST_CHECK(region.BaseAddress == (PVOID)mainModule);
	TEST_CHECK((region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0);

	// The image pointer is checked against the loaded main module before reading headers.
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mainModule;
	TEST_CHECK_EQ(IMAGE_DOS_SIGNATURE, dos->e_magic);
	TEST_CHECK(dos->e_lfanew >= (LONG)sizeof(*dos));
	TEST_CHECK((SIZE_T)dos->e_lfanew <= region.RegionSize - sizeof(DWORD));
	const DWORD *signature = (const DWORD *)((const BYTE *)mainModule + dos->e_lfanew);
	TEST_CHECK_EQ(IMAGE_NT_SIGNATURE, *signature);
	puts("PEB image base: dependency initialization, TLS initialization and entry point verified");
	return 0;
}
