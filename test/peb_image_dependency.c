#include "fixture_peb_image.h"

static FixturePebImageObservation attachObservation;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		observePebImage(&attachObservation, instance);
	}
	return TRUE;
}

__declspec(dllexport) const FixturePebImageObservation *__cdecl getPebImageAttachObservation(void) {
	return &attachObservation;
}
