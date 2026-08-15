#include <windows.h>

static DWORD WINAPI busyWorker(void *parameter) {
	HANDLE started = (HANDLE)parameter;
	SetEvent(started);
	for (;;) {
		(void)GetCurrentThreadId();
		Sleep(0);
	}
	return 0;
}

// Deliberately fault while multiple guest threads are active. Wibo must first
// restore every thread's native TSD base, then terminate the process.
void mainCRTStartup(void) {
	HANDLE started[8];
	HANDLE threads[8];
	for (DWORD index = 0; index < 8; ++index) {
		started[index] = CreateEventA(NULL, TRUE, FALSE, NULL);
		threads[index] = CreateThread(NULL, 0, busyWorker, started[index], 0, NULL);
		if (!started[index] || !threads[index]) {
			ExitProcess(2);
		}
	}
	WaitForMultipleObjects(8, started, TRUE, 5000);
	*(volatile int *)0 = 1;
}
