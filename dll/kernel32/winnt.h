#pragma once

#include "errhandlingapi.h"
#include "types.h"

namespace kernel32 {

void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
PVOID WINAPI RtlPcToFileHeader(PVOID PcValue, GUEST_PTR *BaseOfImage);

} // namespace kernel32
