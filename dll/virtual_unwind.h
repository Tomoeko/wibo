#pragma once

#include "ntdll.h"

#ifdef WIBO_GUEST_64
namespace wibo {
// The caller supplies the current thread's usable stack interval. A failed
// stack read leaves no context suitable for handler dispatch or restoration.
bool virtualUnwindWithStackBounds(DWORD handlerType, ULONGLONG imageBase, ULONGLONG controlPc,
								  RUNTIME_FUNCTION *functionEntry, CONTEXT64 *context, PVOID *handlerData,
								  ULONGLONG *frame, ULONGLONG stackLimit, ULONGLONG stackBase, PVOID *languageHandler);
} // namespace wibo
#endif
