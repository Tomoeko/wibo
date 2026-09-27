#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD WINAPI callMissingImport(LPVOID parameter) {
	(void)parameter;
	GetDateFormatA(LOCALE_USER_DEFAULT, 0, NULL, NULL, NULL, 0);
	ExitProcess(2);
}

void mainCRTStartup(void) {
	// Check the guest's streams without attempting I/O through absent handles.
	if (GetStdHandle(STD_INPUT_HANDLE) || GetStdHandle(STD_OUTPUT_HANDLE) || GetStdHandle(STD_ERROR_HANDLE))
		ExitProcess(21);
	HANDLE thread = CreateThread(NULL, 0, callMissingImport, NULL, 0, NULL);
	if (!thread)
		ExitProcess(22);
	WaitForSingleObject(thread, INFINITE);
	ExitProcess(23);
}
