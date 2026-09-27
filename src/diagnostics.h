#pragma once

namespace wibo {

// Configure host diagnostics independently of the guest's standard handles.
void initializeDiagnostics();
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void diagnosticLog(const char *format, ...);

} // namespace wibo
