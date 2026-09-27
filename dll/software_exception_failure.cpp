#include "software_exception_dispatch.h"

#include "common.h"
#include "kernel32/internal.h"

#include <cstdio>

#ifdef WIBO_GUEST_64
[[noreturn]] void wiboUnsupportedSoftwareExceptionDispatch64(const SoftwareExceptionCapture64 *,
															 const SoftwareExceptionDecision64 *decision) {
	// The dispatcher has returned from every callback and entered host context.
	// General frame handling and secondary exception dispatch are unavailable.
	std::fprintf(stderr, "Unsupported software exception dispatch: kind=%u code=0x%08x status=0x%08x\n",
				 static_cast<unsigned>(decision->kind), decision->originalCode, decision->failureCode);
	kernel32::exitInternal(decision->failureCode);
}
#endif
