#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD WINAPI call_missing_import(LPVOID parameter) {
    (void)parameter;
    /* Deliberately unsupported by Wibo. The Darwin missing-import path must
       terminate directly even when reached from a translated guest worker. */
    GetDateFormatA(LOCALE_USER_DEFAULT, 0, NULL, NULL, NULL, 0);
    ExitProcess(2);
}

void mainCRTStartup(void) {
    HANDLE thread = CreateThread(NULL, 0, call_missing_import, NULL, 0, NULL);
    if (!thread) {
        ExitProcess(3);
    }
    WaitForSingleObject(thread, INFINITE);
    ExitProcess(4);
}
