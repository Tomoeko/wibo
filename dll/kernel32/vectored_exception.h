#pragma once

#include "errhandlingapi.h"

namespace wibo {

using VectoredExceptionInvoker = LONG (*)(PVECTORED_EXCEPTION_HANDLER handler, PEXCEPTION_POINTERS exceptionInfo);

// The invoker supplies the guest callback transition and must return normally.
// Any context restoration must happen after the registry releases its references.
LONG invokeVectoredExceptionHandlers(PEXCEPTION_POINTERS exceptionInfo, VectoredExceptionInvoker invoke);

// Nonreturning transfers must not abandon a traversal's retained entry.
bool hasActiveVectoredExceptionTraversal() noexcept;
bool hasRegisteredVectoredExceptionHandlers();

} // namespace wibo
