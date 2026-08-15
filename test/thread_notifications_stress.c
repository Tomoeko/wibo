#include <windows.h>

static HANDLE g_heap = NULL;
static DWORD g_fls = FLS_OUT_OF_INDEXES;
static CRITICAL_SECTION g_lock;
static volatile LONG g_attach_count = 0;
static volatile LONG g_detach_count = 0;
static volatile LONG g_failure_count = 0;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reserved;

	switch (reason) {
	case DLL_PROCESS_ATTACH:
		InitializeCriticalSection(&g_lock);
		g_heap = HeapCreate(0, 0, 0);
		g_fls = FlsAlloc(NULL);
		return g_heap != NULL && g_fls != FLS_OUT_OF_INDEXES;

	case DLL_THREAD_ATTACH: {
		void *storage = HeapAlloc(g_heap, HEAP_ZERO_MEMORY, 712);
		if (!storage || !FlsSetValue(g_fls, storage)) {
			if (storage)
				HeapFree(g_heap, 0, storage);
			InterlockedIncrement(&g_failure_count);
			break;
		}
		EnterCriticalSection(&g_lock);
		InterlockedIncrement(&g_attach_count);
		LeaveCriticalSection(&g_lock);
		for (unsigned int i = 0; i < 256; ++i) {
			(void)GetCurrentThreadId();
			EnterCriticalSection(&g_lock);
			LeaveCriticalSection(&g_lock);
		}
		break;
	}

	case DLL_THREAD_DETACH: {
		void *storage = FlsGetValue(g_fls);
		// Windows may clear an FLS value before delivering DLL_THREAD_DETACH.
		// A value that remains visible must still belong to the process heap.
		if (storage && !HeapFree(g_heap, 0, storage)) {
			InterlockedIncrement(&g_failure_count);
		}
		FlsSetValue(g_fls, NULL);
		EnterCriticalSection(&g_lock);
		InterlockedIncrement(&g_detach_count);
		LeaveCriticalSection(&g_lock);
		break;
	}

	case DLL_PROCESS_DETACH:
		if (g_fls != FLS_OUT_OF_INDEXES)
			FlsFree(g_fls);
		if (g_heap)
			HeapDestroy(g_heap);
		DeleteCriticalSection(&g_lock);
		break;
	}
	return TRUE;
}

__declspec(dllexport) LONG get_attach_count(void) { return g_attach_count; }
__declspec(dllexport) LONG get_detach_count(void) { return g_detach_count; }
__declspec(dllexport) LONG get_failure_count(void) { return g_failure_count; }
