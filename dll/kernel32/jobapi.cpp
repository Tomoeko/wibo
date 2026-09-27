#include "jobapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "processthreadsapi.h"

#include <unistd.h>

namespace kernel32 {
BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE job, BOOL *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsProcessInJob(%p, %p, %p)\n", process, job, result);
	if (!result) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!isPseudoCurrentProcessHandle(process)) {
		HandleMeta metadata{};
		auto object = wibo::handles().getAs<ProcessObject>(process, &metadata);
		if (!object) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (!(metadata.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION))) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		if (object->pid != getpid() && !object->childProcess) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	if (job != NO_HANDLE) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// This runtime does not create or assign guest jobs, including when launching children.
	*result = FALSE;
	return TRUE;
}
} // namespace kernel32
