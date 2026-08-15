#include <windows.h>
#include <tlhelp32.h>

int main(void) {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE) {
		return 1;
	}

	MODULEENTRY32W entry = {0};
	entry.dwSize = sizeof(entry);
	BOOL foundMainModule = FALSE;
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (entry.hModule == GetModuleHandleW(NULL) && entry.modBaseAddr != NULL && entry.modBaseSize != 0) {
				foundMainModule = TRUE;
				break;
			}
			entry.dwSize = sizeof(entry);
		} while (Module32NextW(snapshot, &entry));
	}

	if (!CloseHandle(snapshot)) {
		return 2;
	}
	return foundMainModule ? 0 : 3;
}
