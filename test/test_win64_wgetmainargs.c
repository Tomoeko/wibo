#include <windows.h>

typedef struct {
	int newmode;
} MsvcrtStartupInfo;

__declspec(dllimport) int __cdecl __wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***environment,
											 int expandWildcards, MsvcrtStartupInfo *startupInfo);

// Exercise the Wine msvcrt startup export used by PE32+ command-line programs
// without depending on a particular guest executable.
void mainCRTStartup(void) {
	int argc = 0;
	wchar_t **argv = NULL;
	wchar_t **environment = NULL;
	MsvcrtStartupInfo startupInfo = {0};
	int result = __wgetmainargs(&argc, &argv, &environment, 1, &startupInfo);
	if (result != 0) {
		ExitProcess(1);
	}
	if (argc < 1 || !argv || !argv[0]) {
		ExitProcess(2);
	}
	ExitProcess(0);
}
