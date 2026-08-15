#include <stdint.h>
#include <windows.h>

void mainCRTStartup(void) {
	const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
	if (!base || *(const uint16_t *)base != 0x5a4d) {
		ExitProcess(1);
	}

	const uint32_t peOffset = *(const uint32_t *)(base + 0x3c);
	const uint8_t *pe = base + peOffset;
	if (*(const uint32_t *)pe != 0x00004550) {
		ExitProcess(2);
	}
	if (*(const uint16_t *)(pe + 0x18) != 0x020b) {
		ExitProcess(3);
	}

	ExitProcess(0);
}
