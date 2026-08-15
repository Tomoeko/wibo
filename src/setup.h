#pragma once

#include "types.h"

#define USER_PRIVILEGE 3

#ifdef __cplusplus
extern "C" {
#endif

bool tebThreadSetup(TEB *teb);
bool tebThreadTeardown(TEB *teb);
#if defined(__APPLE__) && defined(WIBO_GUEST_64)
void tebThreadEmergencyTeardown();
void tebThreadTlsPointerChanged(TEB *teb);
TEB *currentTebForGuestTransition();
void enterGuestContext(TEB *teb);
TEB *enterHostContext();
#endif
void initFpState();

#ifdef __cplusplus
}
#endif
