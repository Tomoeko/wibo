#include <windows.h>

#include "test_assert.h"

#include <stddef.h>

#ifndef _WIN64
#error This fixture requires the x64 exception ABI
#endif

typedef VOID(WINAPI *RaiseExceptionFn)(DWORD, DWORD, DWORD, const ULONG_PTR *);

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

_Static_assert(sizeof(CONTEXT) == 1232 && _Alignof(CONTEXT) == 16, "x64 context layout");
_Static_assert(offsetof(CONTEXT, Rsp) == 152 && offsetof(CONTEXT, Rip) == 248, "x64 control offsets");
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "x64 exception record layout");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionInformation) == 32, "x64 argument offset");
_Static_assert(sizeof(RUNTIME_FUNCTION) == 12, "x64 function table entry width");
_Static_assert(sizeof(struct FixtureDispatcherContext) == 80, "x64 dispatcher layout");
_Static_assert(_Alignof(struct FixtureDispatcherContext) == 8, "x64 dispatcher alignment");
_Static_assert(offsetof(struct FixtureDispatcherContext, ControlPc) == 0, "ControlPc offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, ImageBase) == 8, "ImageBase offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, FunctionEntry) == 16, "FunctionEntry offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, EstablisherFrame) == 24, "EstablisherFrame offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, TargetIp) == 32, "TargetIp offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, ContextRecord) == 40, "ContextRecord offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, LanguageHandler) == 48, "LanguageHandler offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, HandlerData) == 56, "HandlerData offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, HistoryTable) == 64, "HistoryTable offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, ScopeIndex) == 72, "ScopeIndex offset");
_Static_assert(offsetof(struct FixtureDispatcherContext, Fill0) == 76, "Fill0 offset");

extern VOID WINAPI frame_outer(RaiseExceptionFn raise);
extern VOID WINAPI frame_inner(RaiseExceptionFn raise);
extern const BYTE frame_inner_after_raise[], frame_outer_after_inner[];
extern const DWORD frame_inner_handler_data[2], frame_outer_handler_data[2];

ULONG_PTR frame_inner_rsp, frame_outer_rsp;
ULONG_PTR frame_inner_saved_r12, frame_outer_saved_rbx;
ULONG_PTR frame_inner_resumed_rsp, frame_outer_resumed_rsp;
DWORD frame_inner_resumed, frame_outer_resumed;
const ULONG_PTR frame_arguments[2] = {0x123456789abcdef0ULL, 0x0fedcba987654321ULL};

struct HandlerObservation {
	unsigned index;
	BOOL complete;
	EXCEPTION_RECORD record;
	CONTEXT original;
	CONTEXT walking;
	struct FixtureDispatcherContext dispatcher;
	RUNTIME_FUNCTION function;
	ULONG_PTR original_address;
	ULONG_PTR frame;
	DWORD64 marker;
	DWORD error;
};

static struct HandlerObservation observations[2];
static unsigned handler_count;
static BOOL extra_handler;

static void observe_handler(unsigned index, EXCEPTION_RECORD *record, PVOID establisher, CONTEXT *context,
							PVOID dispatcher_pointer) {
	unsigned slot = handler_count++;
	if (slot >= 2) {
		extra_handler = TRUE;
		return;
	}
	struct HandlerObservation *observed = &observations[slot];
	observed->index = index;
	observed->error = GetLastError();
	if (!record || !context || !dispatcher_pointer)
		return;
	const struct FixtureDispatcherContext *dispatcher = dispatcher_pointer;
	if (!dispatcher->ContextRecord || !dispatcher->FunctionEntry)
		return;
	observed->record = *record;
	observed->original = *context;
	observed->walking = *dispatcher->ContextRecord;
	observed->dispatcher = *dispatcher;
	observed->function = *dispatcher->FunctionEntry;
	observed->original_address = (ULONG_PTR)context;
	observed->frame = (ULONG_PTR)establisher;
	ULONG_PTR expected_frame = index == 1 ? frame_inner_rsp : frame_outer_rsp;
	if (observed->frame != expected_frame)
		return;
	memcpy(&observed->marker, (const BYTE *)establisher + 32, sizeof(observed->marker));
	observed->complete = TRUE;
}

EXCEPTION_DISPOSITION NTAPI frame_inner_handler(EXCEPTION_RECORD *record, PVOID establisher, CONTEXT *context,
												PVOID dispatcher) {
	observe_handler(1, record, establisher, context, dispatcher);
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION NTAPI frame_outer_handler(EXCEPTION_RECORD *record, PVOID establisher, CONTEXT *context,
												PVOID dispatcher) {
	observe_handler(2, record, establisher, context, dispatcher);
	return ExceptionContinueExecution;
}

static void check_observation(unsigned slot, ULONG_PTR image_base) {
	const struct HandlerObservation *observed = &observations[slot];
	const struct FixtureDispatcherContext *dispatcher = &observed->dispatcher;
	TEST_CHECK(observed->complete);
	TEST_CHECK_EQ(slot + 1, observed->index);
	TEST_CHECK_EQ(0xe0420a01, observed->record.ExceptionCode);
	TEST_CHECK_EQ(0, observed->record.ExceptionFlags);
	TEST_CHECK_EQ(2, observed->record.NumberParameters);
	for (unsigned index = 0; index < 2; ++index)
		TEST_CHECK_U64_EQ(frame_arguments[index], observed->record.ExceptionInformation[index]);
	TEST_CHECK_U64_EQ(image_base, dispatcher->ImageBase);
	TEST_CHECK_U64_EQ(observed->frame, dispatcher->EstablisherFrame);
	TEST_CHECK_EQ(0x4321, observed->error);
	TEST_CHECK_U64_EQ(0, dispatcher->TargetIp);
	TEST_CHECK_EQ(0, dispatcher->ScopeIndex);
	TEST_CHECK(dispatcher->ContextRecord != (PCONTEXT)observed->original_address);
	TEST_CHECK(dispatcher->ControlPc >= image_base + observed->function.BeginAddress);
	TEST_CHECK(dispatcher->ControlPc < image_base + observed->function.EndAddress);
	ULONG_PTR entry = slot == 0 ? (ULONG_PTR)frame_inner : (ULONG_PTR)frame_outer;
	TEST_CHECK_U64_EQ(entry, image_base + observed->function.BeginAddress);
	const DWORD *data = slot == 0 ? frame_inner_handler_data : frame_outer_handler_data;
	TEST_CHECK(dispatcher->HandlerData == data);
	TEST_CHECK_EQ(0x31415926, data[0]);
	TEST_CHECK_EQ(slot + 1, data[1]);
	TEST_CHECK(dispatcher->LanguageHandler == (slot == 0 ? frame_inner_handler : frame_outer_handler));
	TEST_CHECK_U64_EQ(slot == 0 ? 0x2233445566778899ULL : 0x1122334455667788ULL, observed->marker);
	if (slot == 0) {
		TEST_CHECK_U64_EQ((ULONG_PTR)frame_inner_after_raise, dispatcher->ControlPc);
		TEST_CHECK_U64_EQ((ULONG_PTR)frame_outer_after_inner, observed->walking.Rip);
		TEST_CHECK_U64_EQ(frame_outer_rsp, observed->walking.Rsp);
		TEST_CHECK_U64_EQ(frame_inner_saved_r12, observed->walking.R12);
	} else {
		TEST_CHECK_U64_EQ((ULONG_PTR)frame_outer_after_inner, dispatcher->ControlPc);
		TEST_CHECK_U64_EQ(frame_outer_rsp + 48 + 2 * sizeof(ULONG_PTR), observed->walking.Rsp);
		TEST_CHECK_U64_EQ(frame_outer_saved_rbx, observed->walking.Rbx);
	}
	printf("handler%u: error=%lu original-rip=%llx original-rsp=%llx control-rva=%llx walking-rip=%llx "
		   "walking-rsp=%llx target-ip=%llx scope=%lu\n",
		   observed->index, (unsigned long)observed->error, (unsigned long long)observed->original.Rip,
		   (unsigned long long)observed->original.Rsp, (unsigned long long)(dispatcher->ControlPc - image_base),
		   (unsigned long long)observed->walking.Rip, (unsigned long long)observed->walking.Rsp,
		   (unsigned long long)dispatcher->TargetIp, (unsigned long)dispatcher->ScopeIndex);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "RaiseException");
	RaiseExceptionFn raise;
	_Static_assert(sizeof(exported) == sizeof(raise), "Resolved exception entry pointer width");
	memcpy(&raise, &exported, sizeof(raise));
	TEST_CHECK(raise != NULL);
	ULONG_PTR image_base = (ULONG_PTR)GetModuleHandleA(NULL);
	TEST_CHECK(image_base != 0);
	SetLastError(0x4321);
	frame_outer(raise);
	DWORD resumed_error = GetLastError();
	TEST_CHECK_EQ(0x4321, resumed_error);
	TEST_CHECK_EQ(2, handler_count);
	TEST_CHECK(!extra_handler);
	TEST_CHECK_EQ(1, frame_inner_resumed);
	TEST_CHECK_EQ(1, frame_outer_resumed);
	TEST_CHECK_U64_EQ(frame_inner_rsp, frame_inner_resumed_rsp);
	TEST_CHECK_U64_EQ(frame_outer_rsp, frame_outer_resumed_rsp);
	for (unsigned index = 0; index < 2; ++index)
		check_observation(index, image_base);
	TEST_CHECK_U64_EQ(observations[0].original.Rip, observations[1].original.Rip);
	TEST_CHECK_U64_EQ(observations[0].original.Rsp, observations[1].original.Rsp);
	TEST_CHECK_U64_EQ(observations[0].original.Rbx, observations[1].original.Rbx);
	TEST_CHECK_U64_EQ(observations[0].original.R12, observations[1].original.R12);
	printf("resumed: error=%lu inner-control=%llx outer-control=%llx\n", (unsigned long)resumed_error,
		   (unsigned long long)((ULONG_PTR)frame_inner_after_raise - image_base),
		   (unsigned long long)((ULONG_PTR)frame_outer_after_inner - image_base));
	return 0;
}
