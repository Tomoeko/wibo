#include "ntdll.h"

#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "kernel32/namedpipeapi.h"

namespace ntdll {
NTSTATUS WINAPI NtDeviceIoControlFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
									  PIO_STATUS_BLOCK ioStatus, ULONG control, PVOID input, ULONG inputLength,
									  PVOID output, ULONG outputLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtDeviceIoControlFile(%p, %p, %p, %p, %p, 0x%x, %p, %u, %p, %u)\n", file, event, apcRoutine, apcContext,
			  ioStatus, control, input, inputLength, output, outputLength);
	HandleMeta meta{};
	auto object = wibo::handles().getAs<kernel32::FileObject>(file, &meta);
	if (!object || !object->valid())
		return STATUS_INVALID_HANDLE;
	constexpr ULONG kPipePeek = 0x11400c;
	if (control != kPipePeek || !object->isPipe)
		return STATUS_NOT_SUPPORTED;
	if (!(meta.grantedAccess & FILE_READ_DATA))
		return STATUS_ACCESS_DENIED;
	if (apcRoutine || apcContext)
		return STATUS_NOT_SUPPORTED;
	if (!ioStatus)
		return STATUS_ACCESS_VIOLATION;
	Pin<kernel32::EventObject> completion;
	if (event) {
		completion = wibo::handles().getAs<kernel32::EventObject>(event);
		if (!completion)
			return STATUS_INVALID_HANDLE;
	}
	ULONG_PTR information = 0;
	const NTSTATUS status = kernel32::peekPipeControl(object.get(), output, outputLength, information);
	if (status != STATUS_SUCCESS)
		return status;
	ioStatus->Status = status;
	ioStatus->Information = information;
	if (completion)
		completion->set();
	return status;
}
} // namespace ntdll
