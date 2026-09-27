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
typedef VOID(WINAPI *UnwindExFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID, PCONTEXT, PUNWIND_HISTORY_TABLE);
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
	BYTE beforeCallerContext[16];
	CONTEXT callerContext;
	BYTE afterCallerContext[16];
	UNWIND_HISTORY_TABLE callerHistory;
	CONTEXT innerExContext;
	CONTEXT targetExContext;
	PCONTEXT innerArgument;
	PCONTEXT targetArgument;
	PVOID innerHistory;
	PVOID targetHistory;
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
_Static_assert(offsetof(ProbeState, beforeCallerContext) == 1360, "append-only state extension");
_Static_assert(offsetof(ProbeState, callerContext) == 1376, "aligned caller context offset");
_Static_assert(_Alignof(ProbeState) == 16, "state alignment");

extern VOID WINAPI softwareUnwindRestoreProbe(RaiseFn raise, ProbeState *state);
extern VOID WINAPI softwareUnwindRestoreInner(RaiseFn raise, ProbeState *state);
extern const BYTE software_unwind_restore_landing[], software_unwind_restore_failure[];

static ProbeState states[2];
static RaiseFn raiseSoftware;
static UnwindFn unwindFrames;
static UnwindExFn unwindFramesEx;
static RestoreFn restoreContext;
static BOOL nestedMode;
static BOOL extendedMode;
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
	if (extendedMode && context && dispatcher) {
		state->innerExContext = *context;
		state->innerArgument = context;
		state->innerHistory = dispatcher->HistoryTable;
		if (context != &state->callerContext || dispatcher->HistoryTable != &state->callerHistory)
			state->errors |= 8;
	}
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
		if (extendedMode)
			unwindFramesEx(frame, (PVOID)software_unwind_restore_failure, record, (PVOID)(ULONG_PTR)17,
						   &state->callerContext, &state->callerHistory);
		else
			unwindFrames(frame, (PVOID)software_unwind_restore_failure, record, (PVOID)(ULONG_PTR)17);
		state->unwindReturned = 1;
		return ExceptionContinueExecution;
	}
	++state->targetCleanups;
	append(state, 3);
	state->targetContextFlags = context->ContextFlags;
	state->observedTarget = dispatcher->TargetIp;
	if (extendedMode) {
		state->targetExContext = *context;
		state->targetArgument = context;
		state->targetHistory = dispatcher->HistoryTable;
		if (context != &state->callerContext || dispatcher->HistoryTable != &state->callerHistory)
			state->errors |= 16;
	}
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

static BOOL filledWith(const void *bytes, size_t length, BYTE value) {
	const BYTE *data = bytes;
	for (size_t index = 0; index < length; ++index)
		if (data[index] != value)
			return FALSE;
	return TRUE;
}

static void checkUnselectedGroups(unsigned level, unsigned stage, const CONTEXT *context) {
	BOOL home = filledWith(context, offsetof(CONTEXT, ContextFlags), 0xa5);
	BOOL fpTail = filledWith((const BYTE *)&context->FltSave + 416, 96, 0xa5);
	BOOL vector = filledWith(&context->VectorRegister, sizeof(CONTEXT) - offsetof(CONTEXT, VectorRegister), 0xa5);
	printf("ex=%u mode=%u level=%u stage=%u home-untouched=%u fp-tail-untouched=%u vector-untouched=%u "
		   "dr0=%llx dr1=%llx dr2=%llx dr3=%llx dr6=%llx dr7=%llx\n",
		   (unsigned)extendedMode, (unsigned)nestedMode, level, stage, (unsigned)home, (unsigned)fpTail,
		   (unsigned)vector, (unsigned long long)context->Dr0, (unsigned long long)context->Dr1,
		   (unsigned long long)context->Dr2, (unsigned long long)context->Dr3, (unsigned long long)context->Dr6,
		   (unsigned long long)context->Dr7);
	TEST_CHECK(home && fpTail && vector);
}

static void checkState(unsigned index) {
	const ProbeState *state = &states[index];
	printf("ex=%u mode=%u level=%u searches=%lu inner-cleanups=%lu target-cleanups=%lu trace=%lu "
		   "context-flags=%lx landed=%lu errors=%lx returns=%lu/%lu/%lu failure=%lu\n",
		   (unsigned)extendedMode, (unsigned)nestedMode, index, (unsigned long)state->searches,
		   (unsigned long)state->innerCleanups, (unsigned long)state->targetCleanups, (unsigned long)state->trace,
		   (unsigned long)state->targetContextFlags, (unsigned long)state->landed, (unsigned long)state->errors,
		   (unsigned long)state->innerReturned, (unsigned long)state->unwindReturned,
		   (unsigned long)state->restoreReturned, (unsigned long)state->failureLanding);
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
	if (extendedMode) {
		TEST_CHECK(state->innerArgument == &state->callerContext);
		TEST_CHECK(state->targetArgument == &state->callerContext);
		TEST_CHECK(state->innerHistory == &state->callerHistory);
		TEST_CHECK(state->targetHistory == &state->callerHistory);
		TEST_CHECK(filledWith(state->beforeCallerContext, sizeof(state->beforeCallerContext), 0xa5));
		TEST_CHECK(filledWith(state->afterCallerContext, sizeof(state->afterCallerContext), 0xa5));
		checkUnselectedGroups(index, 1, &state->innerExContext);
		checkUnselectedGroups(index, 2, &state->targetExContext);
		checkUnselectedGroups(index, 3, &state->callerContext);
	}
}

static void run(BOOL extended, BOOL nested) {
	memset(states, 0, sizeof(states));
	for (unsigned index = 0; index < 2; ++index) {
		memset(states[index].beforeCallerContext, 0xa5, sizeof(states[index].beforeCallerContext));
		memset(&states[index].callerContext, 0xa5, sizeof(states[index].callerContext));
		memset(states[index].afterCallerContext, 0xa5, sizeof(states[index].afterCallerContext));
	}
	unmatchedFrame = 0;
	nestedMode = nested;
	extendedMode = extended;
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
	FARPROC unwindExEntry = GetProcAddress(kernel, "RtlUnwindEx");
	FARPROC restoreEntry = GetProcAddress(native, "RtlRestoreContext");
	_Static_assert(sizeof(raiseEntry) == sizeof(raiseSoftware), "raise entry pointer width");
	_Static_assert(sizeof(unwindEntry) == sizeof(unwindFrames), "unwind entry pointer width");
	_Static_assert(sizeof(unwindExEntry) == sizeof(unwindFramesEx), "extended unwind entry pointer width");
	_Static_assert(sizeof(restoreEntry) == sizeof(restoreContext), "restore entry pointer width");
	memcpy(&raiseSoftware, &raiseEntry, sizeof(raiseSoftware));
	memcpy(&unwindFrames, &unwindEntry, sizeof(unwindFrames));
	memcpy(&unwindFramesEx, &unwindExEntry, sizeof(unwindFramesEx));
	memcpy(&restoreContext, &restoreEntry, sizeof(restoreContext));
	TEST_CHECK(raiseSoftware != NULL && unwindFrames != NULL && unwindFramesEx != NULL && restoreContext != NULL);
	run(FALSE, FALSE);
	run(FALSE, TRUE);
	run(TRUE, FALSE);
	run(TRUE, TRUE);
	return 0;
}
