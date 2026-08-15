#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

#include "test_assert.h"

enum { WORKER_COUNT = 12 };

#ifdef _WIN64
extern DWORD _tls_index;
#endif

#ifdef _WIN64
static void *read_teb(void) {
	void *teb;
	__asm__("movq %%gs:0x30, %0" : "=r"(teb));
	return teb;
}

static void *read_static_tls_vector(void) {
	void *tls;
	__asm__("movq %%gs:0x58, %0" : "=r"(tls));
	return tls;
}

static void *read_process_image_tls_block(void) {
	void **const tls = (void **)read_static_tls_vector();
	return tls ? tls[_tls_index] : NULL;
}
#endif

static DWORD WINAPI worker_proc(LPVOID parameter) {
	(void)parameter;
#ifdef _WIN64
	void *const teb = read_teb();
	void *const tls = read_static_tls_vector();
	if (!teb || !tls)
		return 1;
	void *const process_image_tls = read_process_image_tls_block();
	if (!process_image_tls)
		return 3;
#endif
	for (unsigned int i = 0; i < 20000; ++i) {
		(void)GetCurrentThreadId();
#ifdef _WIN64
		if (read_teb() != teb || read_static_tls_vector() != tls)
			return 2;
		if (read_process_image_tls_block() != process_image_tls)
			return 4;
#endif
	}
	return 0;
}

static FARPROC load_export(HMODULE module, const char *name) { return GetProcAddress(module, name); }

int main(void) {
	typedef LONG (*get_count_fn)(void);

	HMODULE module = LoadLibraryA("thread_notifications_stress.dll");
	TEST_CHECK_MSG(module != NULL, "LoadLibraryA failed: %lu", (unsigned long)GetLastError());

	get_count_fn get_attach_count = (get_count_fn)(uintptr_t)load_export(module, "get_attach_count");
	get_count_fn get_detach_count = (get_count_fn)(uintptr_t)load_export(module, "get_detach_count");
	get_count_fn get_failure_count = (get_count_fn)(uintptr_t)load_export(module, "get_failure_count");
	TEST_CHECK(get_attach_count != NULL);
	TEST_CHECK(get_detach_count != NULL);
	TEST_CHECK(get_failure_count != NULL);

	HANDLE workers[WORKER_COUNT] = {0};
	for (unsigned int i = 0; i < WORKER_COUNT; ++i) {
		workers[i] = CreateThread(NULL, 0, worker_proc, NULL, CREATE_SUSPENDED, NULL);
		TEST_CHECK_MSG(workers[i] != NULL, "CreateThread %u failed: %lu", i, (unsigned long)GetLastError());
	}
	for (unsigned int i = 0; i < WORKER_COUNT; ++i) {
		TEST_CHECK_MSG(ResumeThread(workers[i]) == 1, "ResumeThread %u failed: %lu", i, (unsigned long)GetLastError());
	}

	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(WORKER_COUNT, workers, TRUE, 10000));
	for (unsigned int i = 0; i < WORKER_COUNT; ++i) {
		DWORD exit_code = STILL_ACTIVE;
		TEST_CHECK(GetExitCodeThread(workers[i], &exit_code));
		TEST_CHECK_EQ(0, exit_code);
		TEST_CHECK(CloseHandle(workers[i]));
	}

	TEST_CHECK_EQ(WORKER_COUNT, get_attach_count());
	TEST_CHECK_EQ(WORKER_COUNT, get_detach_count());
	TEST_CHECK_EQ(0, get_failure_count());
	TEST_CHECK(FreeLibrary(module));
	return EXIT_SUCCESS;
}
