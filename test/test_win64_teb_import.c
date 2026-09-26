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

static DWORD read_last_error(void) {
	DWORD error;
	__asm__ volatile("movl %%gs:0x68, %0" : "=r"(error));
	return error;
}

static void write_last_error(DWORD error) { __asm__ volatile("movl %0, %%gs:0x68" : : "r"(error) : "memory"); }

static DWORD check_last_error(DWORD marker) {
	volatile DWORD *field = (volatile DWORD *)((char *)read_teb() + 0x68);
	SetLastError(marker);
	if (read_last_error() != marker || *field != marker || GetLastError() != marker)
		return 10;
	write_last_error(marker + 1);
	if (GetLastError() != marker + 1 || *field != marker + 1 || read_last_error() != marker + 1)
		return 11;
	*field = marker + 2;
	if (GetLastError() != marker + 2 || read_last_error() != marker + 2)
		return 12;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (GetFileAttributesExW(L"Z:\\__wibo_last_error_missing__\\absent.bin", GetFileExInfoStandard, &attributes))
		return 13;
	DWORD error = read_last_error();
	if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) || GetLastError() != error || *field != error)
		return 14;
	DWORD index = TlsAlloc();
	if (index == TLS_OUT_OF_INDEXES || !TlsSetValue(index, (void *)(ULONG_PTR)error) ||
		(ULONG_PTR)TlsGetValue(index) != error || !TlsFree(index))
		return 15;
	return 0;
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
	DWORD error = check_last_error(702);
	return error ? error : check_context();
}

void mainCRTStartup(void) {
	mainTeb = read_teb();
	processPeb = read_peb();
	if (!mainTeb || !processPeb)
		ExitProcess(1);
	DWORD error = check_last_error(701);
	if (!error)
		error = check_context();
	if (error)
		ExitProcess(error);
	INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	if (!InitOnceExecuteOnce(&once, initialized, NULL, NULL))
		ExitProcess(2);
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
	if (!thread || ResumeThread(thread) == (DWORD)-1)
		ExitProcess(3);
	SetLastError(703);
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &error))
		ExitProcess(4);
	if (error)
		ExitProcess(error);
	if (read_last_error() != 703 || GetLastError() != 703)
		ExitProcess(16);
	if (!CloseHandle(thread) || check_context())
		ExitProcess(5);
	ExitProcess(0);
}
