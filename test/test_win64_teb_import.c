#include <windows.h>

static void *read_teb(void) {
	void *teb;
	__asm__("movq %%gs:0x30, %0" : "=r"(teb));
	return teb;
}

static void *read_static_tls_vector(void) {
	void *tls;
	__asm__("movq %%gs:0x58, %0" : "=r"(tls));
	return tls;
}

void mainCRTStartup(void) {
	void *before = read_teb();
	void *tlsBefore = read_static_tls_vector();
	if (!before) {
		ExitProcess(1);
	}
	if (!tlsBefore) {
		ExitProcess(4);
	}
	(void)GetCurrentProcessId();
	void *after = read_teb();
	void *tlsAfter = read_static_tls_vector();
	if (!after) {
		ExitProcess(2);
	}
	if (after != before) {
		ExitProcess(3);
	}
	if (tlsAfter != tlsBefore) {
		ExitProcess(5);
	}
	ExitProcess(0);
}
