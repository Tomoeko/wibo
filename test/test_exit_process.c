#include <windows.h>

// Avoid the CRT so this fixture isolates the loader-to-ExitProcess lifecycle.
// In particular, Wibo must restore any host thread state before _exit terminates
// the translated process.
void mainCRTStartup(void) {
	ExitProcess(0);
}
