#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_assert.h"

#ifndef _WIN64
#error This fixture requires the x64 exception ABI
#endif

typedef VOID(WINAPI *RaiseFn)(DWORD, DWORD, DWORD, const ULONG_PTR *);
typedef VOID(WINAPI *UnwindFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID);
typedef VOID(__cdecl *RestoreFn)(PCONTEXT, PEXCEPTION_RECORD);

struct FixtureDispatcherContext {
	DWORD64 ControlPc;
	DWORD64 ImageBase;
	PRUNTIME_FUNCTION FunctionEntry;
	DWORD64 EstablisherFrame;
	DWORD64 TargetIp;
	PCONTEXT ContextRecord;
	PEXCEPTION_ROUTINE LanguageHandler;
	PVOID HandlerData;
	PVOID HistoryTable;
	DWORD ScopeIndex;
	DWORD Fill0;
};

typedef struct {
	ULONG_PTR outerRsp;
	ULONG_PTR innerRsp;
	ULONG_PTR landedRsp;
	ULONG_PTR landedRax;
	ULONG_PTR landedRbx;
	ULONG_PTR landedR12;
	ULONGLONG landedXmm6[2];
	DWORD landed;
	DWORD innerReturned;
	DWORD failureLanding;
	DWORD unwindReturned;
	DWORD restoreReturned;
	DWORD searches;
	DWORD innerCleanups;
	DWORD targetCleanups;
	DWORD trace;
	DWORD errors;
	DWORD targetContextFlags;
	DWORD nestedReturned;
	ULONG_PTR observedTarget;
	CONTEXT selectedContext;
} ProbeState;

_Static_assert(sizeof(CONTEXT) == 1232 && _Alignof(CONTEXT) == 16, "x64 context layout");
_Static_assert(sizeof(struct FixtureDispatcherContext) == 80, "x64 dispatcher size");
_Static_assert(offsetof(struct FixtureDispatcherContext, ContextRecord) == 40, "dispatcher context offset");
_Static_assert(offsetof(ProbeState, landedXmm6) == 48, "landing vector offset");
_Static_assert(offsetof(ProbeState, landed) == 64, "landing marker offset");
_Static_assert(offsetof(ProbeState, innerReturned) == 68, "return marker offset");
_Static_assert(offsetof(ProbeState, failureLanding) == 72, "failure landing offset");
_Static_assert(offsetof(ProbeState, trace) == 96, "trace offset");
_Static_assert(offsetof(ProbeState, selectedContext) == 128, "selected context offset");
_Static_assert(sizeof(ProbeState) == 1360 && _Alignof(ProbeState) == 16, "state layout");

extern VOID WINAPI softwareUnwindRestoreProbe(RaiseFn raise, ProbeState *state);
extern VOID WINAPI softwareUnwindRestoreInner(RaiseFn raise, ProbeState *state);
extern const BYTE software_unwind_restore_landing[], software_unwind_restore_failure[];

static ProbeState states[2];
static RaiseFn raiseSoftware;
static UnwindFn unwindFrames;
static RestoreFn restoreContext;
static BOOL nestedMode;
static DWORD unmatchedFrame;

const ULONGLONG software_unwind_outer_xmm6[2]
	__attribute__((aligned(16))) = {UINT64_C(0x1020304050607080), UINT64_C(0x90a0b0c0d0e0f000)};
static const ULONGLONG selectedXmm6[2] = {UINT64_C(0x1122334455667788), UINT64_C(0x99aabbccddeeff00)};

static ProbeState *findState(PVOID frame, BOOL outer) {
	for (unsigned index = 0; index < 2; ++index) {
		ULONG_PTR expected = outer ? states[index].outerRsp : states[index].innerRsp;
		if (expected && expected == (ULONG_PTR)frame)
			return &states[index];
	}
	unmatchedFrame = 1;
	return NULL;
}

static void append(ProbeState *state, DWORD digit) { state->trace = state->trace * 10 + digit; }

EXCEPTION_DISPOSITION NTAPI software_unwind_inner_handler(EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context,
														  PVOID dispatcher_pointer) {
	ProbeState *state = findState(frame, FALSE);
	if (!state)
		return ExceptionContinueSearch;
	++state->innerCleanups;
	append(state, 2);
	const struct FixtureDispatcherContext *dispatcher = dispatcher_pointer;
	if (!record || record->ExceptionCode != 0xe0420a03 || record->ExceptionFlags != EXCEPTION_UNWINDING || !context ||
		!dispatcher || dispatcher->ContextRecord != context || context->Rsp != state->innerRsp)
		state->errors |= 1;
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION NTAPI software_unwind_outer_handler(EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context,
														  PVOID dispatcher_pointer) {
	ProbeState *state = findState(frame, TRUE);
	if (!state)
		return ExceptionContinueSearch;
	struct FixtureDispatcherContext *dispatcher = dispatcher_pointer;
	if (!record || record->ExceptionCode != 0xe0420a03 || !context || !dispatcher) {
		state->errors |= 2;
		return ExceptionContinueExecution;
	}
	if (!(record->ExceptionFlags & EXCEPTION_UNWINDING)) {
		++state->searches;
		append(state, 1);
		if (nestedMode && state == &states[0] && state->searches == 1) {
			// This inner transfer retains the still-running outer personality.
			softwareUnwindRestoreProbe(raiseSoftware, &states[1]);
			state->nestedReturned = 1;
		}
		unwindFrames(frame, (PVOID)software_unwind_restore_failure, record, (PVOID)(ULONG_PTR)17);
		state->unwindReturned = 1;
		return ExceptionContinueExecution;
	}
	++state->targetCleanups;
	append(state, 3);
	state->targetContextFlags = context->ContextFlags;
	state->observedTarget = dispatcher->TargetIp;
	if (record->ExceptionFlags != (EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND) ||
		dispatcher->ContextRecord != context || dispatcher->EstablisherFrame != state->outerRsp ||
		context->Rsp != state->outerRsp || context->ContextFlags != (CONTEXT_FULL | CONTEXT_SEGMENTS)) {
		state->errors |= 4;
		return ExceptionContinueSearch;
	}
	// Native failure remains a valid landing, so an API return never jumps to an invalid PC.
	state->selectedContext = *context;
	state->selectedContext.Rip = (ULONG_PTR)software_unwind_restore_landing;
	state->selectedContext.Rax = 17;
	state->selectedContext.Rbx = UINT64_C(0x2132435465768798);
	memcpy(&state->selectedContext.Xmm6, selectedXmm6, sizeof(selectedXmm6));
	restoreContext(&state->selectedContext, NULL);
	state->restoreReturned = 1;
	return ExceptionContinueSearch;
}

static void checkState(unsigned index) {
	const ProbeState *state = &states[index];
	printf("mode=%u level=%u searches=%lu inner-cleanups=%lu target-cleanups=%lu trace=%lu "
		   "context-flags=%lx landed=%lu errors=%lx returns=%lu/%lu/%lu failure=%lu\n",
		   (unsigned)nestedMode, index, (unsigned long)state->searches, (unsigned long)state->innerCleanups,
		   (unsigned long)state->targetCleanups, (unsigned long)state->trace, (unsigned long)state->targetContextFlags,
		   (unsigned long)state->landed, (unsigned long)state->errors, (unsigned long)state->innerReturned,
		   (unsigned long)state->unwindReturned, (unsigned long)state->restoreReturned,
		   (unsigned long)state->failureLanding);
	TEST_CHECK_EQ(0, state->errors);
	TEST_CHECK_EQ(0, state->innerReturned);
	TEST_CHECK_EQ(0, state->failureLanding);
	TEST_CHECK_EQ(0, state->unwindReturned);
	TEST_CHECK_EQ(0, state->restoreReturned);
	TEST_CHECK_EQ(1, state->landed);
	TEST_CHECK_EQ(1, state->searches);
	TEST_CHECK_EQ(1, state->innerCleanups);
	TEST_CHECK_EQ(1, state->targetCleanups);
	TEST_CHECK_EQ(1234, state->trace);
	TEST_CHECK_EQ(CONTEXT_FULL | CONTEXT_SEGMENTS, state->targetContextFlags);
	TEST_CHECK_U64_EQ((ULONG_PTR)software_unwind_restore_failure, state->observedTarget);
	TEST_CHECK_U64_EQ(state->outerRsp, state->landedRsp);
	TEST_CHECK_U64_EQ(17, state->landedRax);
	TEST_CHECK_U64_EQ(UINT64_C(0x2132435465768798), state->landedRbx);
	TEST_CHECK_U64_EQ((ULONG_PTR)state, state->landedR12);
	TEST_CHECK(memcmp(selectedXmm6, state->landedXmm6, sizeof(selectedXmm6)) == 0);
}

static void run(BOOL nested) {
	memset(states, 0, sizeof(states));
	unmatchedFrame = 0;
	nestedMode = nested;
	SetLastError(0x4321);
	softwareUnwindRestoreProbe(raiseSoftware, &states[0]);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0x4321, error);
	TEST_CHECK_EQ(0, unmatchedFrame);
	checkState(0);
	TEST_CHECK_EQ(nested ? 1 : 0, states[0].nestedReturned);
	if (nested) {
		checkState(1);
		TEST_CHECK_EQ(0, states[1].nestedReturned);
	}
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	HMODULE native = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(kernel != NULL && native != NULL);
	FARPROC raiseEntry = GetProcAddress(kernel, "RaiseException");
	FARPROC unwindEntry = GetProcAddress(kernel, "RtlUnwind");
	FARPROC restoreEntry = GetProcAddress(native, "RtlRestoreContext");
	_Static_assert(sizeof(raiseEntry) == sizeof(raiseSoftware), "raise entry pointer width");
	_Static_assert(sizeof(unwindEntry) == sizeof(unwindFrames), "unwind entry pointer width");
	_Static_assert(sizeof(restoreEntry) == sizeof(restoreContext), "restore entry pointer width");
	memcpy(&raiseSoftware, &raiseEntry, sizeof(raiseSoftware));
	memcpy(&unwindFrames, &unwindEntry, sizeof(unwindFrames));
	memcpy(&restoreContext, &restoreEntry, sizeof(restoreContext));
	TEST_CHECK(raiseSoftware != NULL && unwindFrames != NULL && restoreContext != NULL);
	run(FALSE);
	run(TRUE);
	return 0;
}
