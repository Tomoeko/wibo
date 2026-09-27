#ifndef WIBO_FIXTURE_PEB_IMAGE_H
#define WIBO_FIXTURE_PEB_IMAGE_H

#include <stddef.h>
#include <windows.h>

typedef struct {
	BYTE flags[4];
	ULONG_PTR mutant;
	ULONG_PTR imageBase;
	ULONG_PTR loaderData;
	ULONG_PTR processParameters;
} FixturePebPrefix;

#ifdef _WIN64
_Static_assert(offsetof(FixturePebPrefix, imageBase) == 0x10, "PEB image base offset");
_Static_assert(offsetof(FixturePebPrefix, loaderData) == 0x18, "PEB loader offset");
#else
_Static_assert(offsetof(FixturePebPrefix, imageBase) == 0x08, "PEB image base offset");
_Static_assert(offsetof(FixturePebPrefix, loaderData) == 0x0c, "PEB loader offset");
#endif

typedef struct {
	ULONG calls;
	ULONG_PTR teb;
	ULONG_PTR peb;
	ULONG_PTR imageBase;
	ULONG_PTR mainModule;
	ULONG_PTR dependency;
} FixturePebImageObservation;

static inline void observePebImage(FixturePebImageObservation *observation, HINSTANCE dependency) {
	ULONG_PTR teb;
#ifdef _WIN64
	__asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
	const size_t pebOffset = 0x60;
#else
	__asm__ volatile("movl %%fs:0x18, %0" : "=r"(teb));
	const size_t pebOffset = 0x30;
#endif
	observation->calls++;
	observation->teb = teb;
	observation->peb = 0;
	observation->imageBase = 0;
	observation->mainModule = (ULONG_PTR)GetModuleHandleA(NULL);
	observation->dependency = (ULONG_PTR)dependency;
	if (teb) {
		observation->peb = *(const volatile ULONG_PTR *)(teb + pebOffset);
		if (observation->peb) {
			const volatile FixturePebPrefix *peb = (const volatile FixturePebPrefix *)observation->peb;
			observation->imageBase = peb->imageBase;
		}
	}
}

#endif
