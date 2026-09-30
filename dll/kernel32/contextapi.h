#pragma once

#include "thread_context.h"

namespace kernel32 {
BOOL WINAPI InitializeContext(PVOID buffer, DWORD contextFlags, GUEST_PTR *context, PDWORD contextLength);
BOOL WINAPI GetXStateFeaturesMask(const CONTEXT *context, ULONGLONG *featureMask);
BOOL WINAPI SetXStateFeaturesMask(CONTEXT *context, ULONGLONG featureMask);
PVOID WINAPI LocateXStateFeature(CONTEXT *context, DWORD featureId, PDWORD length);
} // namespace kernel32
