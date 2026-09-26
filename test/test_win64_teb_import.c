#include <windows.h>

static void *read_teb(void) {
	void *teb;
	__asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
	return teb;
}

static void *read_static_tls_vector(void) {
	void *tls;
	__asm__ volatile("movq %%gs:0x58, %0" : "=r"(tls));
	return tls;
}

static void *read_peb(void) {
	void *peb;
	__asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
	return peb;
}

static void *processPeb;
static void *mainTeb;

static DWORD check_context(void) {
	void *teb = read_teb();
	if (!teb || !read_static_tls_vector() || read_peb() != processPeb || *(void **)((char *)teb + 0x60) != processPeb)
		return 6;
	for (unsigned i = 0; i < 20; ++i) {
		SYSTEMTIME now;
		GetLocalTime(&now);
		if (now.wYear < 1601 || read_peb() != processPeb || read_teb() != teb)
			return 7;
	}
	return 0;
}

static BOOL CALLBACK initialized(INIT_ONCE *once, void *parameter, void **context) {
	(void)once;
	(void)parameter;
	(void)context;
	DWORD error = check_context();
	if (error)
		ExitProcess(error);
	return TRUE;
}

static DWORD WINAPI worker(void *unused) {
	(void)unused;
	if (read_teb() == mainTeb)
		return 8;
	return check_context();
}

void mainCRTStartup(void) {
	mainTeb = read_teb();
	processPeb = read_peb();
	if (!mainTeb || !processPeb)
		ExitProcess(1);
	DWORD error = check_context();
	if (error)
		ExitProcess(error);
	INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	if (!InitOnceExecuteOnce(&once, initialized, NULL, NULL))
		ExitProcess(2);
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
	if (!thread || ResumeThread(thread) == (DWORD)-1)
		ExitProcess(3);
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &error))
		ExitProcess(4);
	if (error)
		ExitProcess(error);
	if (!CloseHandle(thread) || check_context())
		ExitProcess(5);
	ExitProcess(0);
}
