#pragma once

#include "context_x64_offsets.h"
#include "kernel32/thread_context.h"

static_assert(sizeof(CONTEXT64) == 1232);
static_assert(alignof(CONTEXT64) == 16);
static_assert(sizeof(XMM_SAVE_AREA32) == 512);

#define WIBO_CHECK_CONTEXT_OFFSET(field, offset) static_assert(offsetof(CONTEXT64, field) == offset)
WIBO_CHECK_CONTEXT_OFFSET(ContextFlags, WIBO_CTX64_FLAGS);
WIBO_CHECK_CONTEXT_OFFSET(MxCsr, WIBO_CTX64_MXCSR);
WIBO_CHECK_CONTEXT_OFFSET(SegCs, WIBO_CTX64_CS);
WIBO_CHECK_CONTEXT_OFFSET(SegDs, WIBO_CTX64_DS);
WIBO_CHECK_CONTEXT_OFFSET(SegEs, WIBO_CTX64_ES);
WIBO_CHECK_CONTEXT_OFFSET(SegFs, WIBO_CTX64_FS);
WIBO_CHECK_CONTEXT_OFFSET(SegGs, WIBO_CTX64_GS);
WIBO_CHECK_CONTEXT_OFFSET(SegSs, WIBO_CTX64_SS);
WIBO_CHECK_CONTEXT_OFFSET(EFlags, WIBO_CTX64_EFLAGS);
WIBO_CHECK_CONTEXT_OFFSET(Rax, WIBO_CTX64_RAX);
WIBO_CHECK_CONTEXT_OFFSET(Rcx, WIBO_CTX64_RCX);
WIBO_CHECK_CONTEXT_OFFSET(Rdx, WIBO_CTX64_RDX);
WIBO_CHECK_CONTEXT_OFFSET(Rbx, WIBO_CTX64_RBX);
WIBO_CHECK_CONTEXT_OFFSET(Rsp, WIBO_CTX64_RSP);
WIBO_CHECK_CONTEXT_OFFSET(Rbp, WIBO_CTX64_RBP);
WIBO_CHECK_CONTEXT_OFFSET(Rsi, WIBO_CTX64_RSI);
WIBO_CHECK_CONTEXT_OFFSET(Rdi, WIBO_CTX64_RDI);
WIBO_CHECK_CONTEXT_OFFSET(R8, WIBO_CTX64_R8);
WIBO_CHECK_CONTEXT_OFFSET(R9, WIBO_CTX64_R9);
WIBO_CHECK_CONTEXT_OFFSET(R10, WIBO_CTX64_R10);
WIBO_CHECK_CONTEXT_OFFSET(R11, WIBO_CTX64_R11);
WIBO_CHECK_CONTEXT_OFFSET(R12, WIBO_CTX64_R12);
WIBO_CHECK_CONTEXT_OFFSET(R13, WIBO_CTX64_R13);
WIBO_CHECK_CONTEXT_OFFSET(R14, WIBO_CTX64_R14);
WIBO_CHECK_CONTEXT_OFFSET(R15, WIBO_CTX64_R15);
WIBO_CHECK_CONTEXT_OFFSET(Rip, WIBO_CTX64_RIP);
WIBO_CHECK_CONTEXT_OFFSET(FltSave, WIBO_CTX64_FXSAVE);
#undef WIBO_CHECK_CONTEXT_OFFSET

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
extern "C" {

// Capture the caller's continuation before any host context transition.
void GUEST_STDCALL wiboCaptureContext64(CONTEXT64 *context);

// Restore valid integer, control, and legacy x87/SSE state. The caller must
// release host resources and establish guest context before entering here.
// The target stack needs one writable return slot below its restored RSP.
// Floating-point control state is taken from FltSave, including its MXCSR.
// Segment selectors and TLS bases are preserved; extended/debug state is unsupported.
[[noreturn]] void GUEST_STDCALL wiboRestoreContext64(const CONTEXT64 *context);
}
#endif
