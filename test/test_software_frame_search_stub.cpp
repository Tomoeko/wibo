#include "software_exception_frame.h"

// Isolated decision/TEB fixtures retain their original vectored-handler scope.
// The public PE fixture exercises the production frame-search implementation.
bool wiboSearchSoftwareExceptionFrames64(const SoftwareExceptionCapture64 *, SoftwareExceptionDecision64 *,
										 SoftwareExceptionFrameActivation64 *) {
	return false;
}

namespace kernel32 {
LPTOP_LEVEL_EXCEPTION_FILTER currentUnhandledExceptionFilter() { return nullptr; }
} // namespace kernel32
