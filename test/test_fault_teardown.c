// Deliberately fault without a CRT or Win32 API call. On macOS/Win64 this
// verifies that Wibo's fatal-signal path clears Rosetta's reserved TEB slot
// before terminating the host process.
void mainCRTStartup(void) {
	*(volatile int *)0 = 1;
}
