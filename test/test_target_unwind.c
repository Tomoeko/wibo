#include <windows.h>

#include "test_assert.h"

#include <stddef.h>

#ifndef _WIN64
#error This fixture requires the x64 exception ABI
#endif

typedef VOID(WINAPI *RtlUnwindFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID);

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

extern VOID WINAPI target_unwind_outer(RtlUnwindFn unwind);
extern VOID WINAPI target_unwind_inner(RtlUnwindFn unwind);
extern const BYTE target_unwind_inner_after_call[], target_unwind_outer_after_inner[], target_unwind_landing[];
extern const DWORD target_unwind_inner_data[2], target_unwind_outer_data[2];

ULONG_PTR target_unwind_inner_rsp, target_unwind_outer_rsp;
ULONG_PTR target_unwind_landed_rsp, target_unwind_landed_rax;
ULONG_PTR target_unwind_landed_rbx, target_unwind_landed_r12;
ULONGLONG target_unwind_landed_xmm6[2];
DWORD target_unwind_returned, target_unwind_inner_returned, target_unwind_landed;
EXCEPTION_RECORD target_unwind_record;
const ULONG_PTR target_unwind_value = 0x123456789abcdef0ULL;
const ULONGLONG target_unwind_outer_xmm6[2]
	__attribute__((aligned(16))) = {0x1020304050607080ULL, 0x90a0b0c0d0e0f000ULL};
const ULONGLONG target_unwind_inner_xmm6[2]
	__attribute__((aligned(16))) = {0x8877665544332211ULL, 0x00ffeeddccbbaa99ULL};

struct HandlerObservation {
	unsigned index;
	BOOL complete;
	EXCEPTION_RECORD record;
	CONTEXT argument_context;
	CONTEXT dispatcher_context;
	struct FixtureDispatcherContext dispatcher;
	RUNTIME_FUNCTION function;
	ULONG_PTR argument_address;
	ULONG_PTR record_address;
	ULONG_PTR frame;
	ULONGLONG marker;
	DWORD error;
};

static struct HandlerObservation observations[2];
static unsigned handler_count;
static BOOL extra_handler;
static BOOL change_scope_index;
static unsigned scope_changes;

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
	observed->argument_context = *context;
	observed->dispatcher_context = *dispatcher->ContextRecord;
	observed->dispatcher = *dispatcher;
	observed->function = *dispatcher->FunctionEntry;
	observed->argument_address = (ULONG_PTR)context;
	observed->record_address = (ULONG_PTR)record;
	observed->frame = (ULONG_PTR)establisher;
	ULONG_PTR expected_frame = index == 1 ? target_unwind_inner_rsp : target_unwind_outer_rsp;
	if (observed->frame != expected_frame)
		return;
	memcpy(&observed->marker, (const BYTE *)establisher + 32, sizeof(observed->marker));
	observed->complete = TRUE;
	if (change_scope_index) {
		((struct FixtureDispatcherContext *)dispatcher_pointer)->ScopeIndex = index == 1 ? 73 : MAXDWORD;
		++scope_changes;
	}
}

EXCEPTION_DISPOSITION NTAPI target_unwind_inner_handler(EXCEPTION_RECORD *record, PVOID establisher, CONTEXT *context,
														PVOID dispatcher) {
	observe_handler(1, record, establisher, context, dispatcher);
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION NTAPI target_unwind_outer_handler(EXCEPTION_RECORD *record, PVOID establisher, CONTEXT *context,
														PVOID dispatcher) {
	observe_handler(2, record, establisher, context, dispatcher);
	return ExceptionContinueSearch;
}

static void check_observation(unsigned slot, ULONG_PTR image_base) {
	const struct HandlerObservation *observed = &observations[slot];
	const struct FixtureDispatcherContext *dispatcher = &observed->dispatcher;
	TEST_CHECK(observed->complete);
	TEST_CHECK_EQ(slot + 1, observed->index);
	TEST_CHECK_U64_EQ((ULONG_PTR)&target_unwind_record, observed->record_address);
	TEST_CHECK_EQ(0xe0420a02, observed->record.ExceptionCode);
	TEST_CHECK_EQ(EXCEPTION_UNWINDING | (slot ? EXCEPTION_TARGET_UNWIND : 0), observed->record.ExceptionFlags);
	TEST_CHECK(observed->record.ExceptionAddress == (PVOID)target_unwind_inner_after_call);
	TEST_CHECK_EQ(2, observed->record.NumberParameters);
	TEST_CHECK_U64_EQ(0x0123456789abcdefULL, observed->record.ExceptionInformation[0]);
	TEST_CHECK_U64_EQ(0xfedcba9876543210ULL, observed->record.ExceptionInformation[1]);
	TEST_CHECK_U64_EQ(image_base, dispatcher->ImageBase);
	TEST_CHECK_U64_EQ(observed->frame, dispatcher->EstablisherFrame);
	TEST_CHECK_EQ(0x4321, observed->error);
	TEST_CHECK_U64_EQ((ULONG_PTR)target_unwind_landing, dispatcher->TargetIp);
	TEST_CHECK_EQ(0, dispatcher->ScopeIndex);
	TEST_CHECK_U64_EQ(observed->argument_address, (ULONG_PTR)dispatcher->ContextRecord);
	TEST_CHECK(memcmp(&observed->argument_context, &observed->dispatcher_context, sizeof(CONTEXT)) == 0);
	TEST_CHECK(dispatcher->ControlPc >= image_base + observed->function.BeginAddress);
	TEST_CHECK(dispatcher->ControlPc < image_base + observed->function.EndAddress);
	ULONG_PTR entry = slot == 0 ? (ULONG_PTR)target_unwind_inner : (ULONG_PTR)target_unwind_outer;
	TEST_CHECK_U64_EQ(entry, image_base + observed->function.BeginAddress);
	const DWORD *data = slot == 0 ? target_unwind_inner_data : target_unwind_outer_data;
	TEST_CHECK(dispatcher->HandlerData == data);
	TEST_CHECK_EQ(0x27182818, data[0]);
	TEST_CHECK_EQ(slot + 1, data[1]);
	TEST_CHECK(dispatcher->LanguageHandler == (slot == 0 ? target_unwind_inner_handler : target_unwind_outer_handler));
	TEST_CHECK_U64_EQ(slot == 0 ? 0x2233445566778899ULL : 0x1122334455667788ULL, observed->marker);
	ULONG_PTR control =
		slot == 0 ? (ULONG_PTR)target_unwind_inner_after_call : (ULONG_PTR)target_unwind_outer_after_inner;
	ULONG_PTR frame = slot == 0 ? target_unwind_inner_rsp : target_unwind_outer_rsp;
	TEST_CHECK_U64_EQ(control, dispatcher->ControlPc);
	TEST_CHECK_U64_EQ(control, observed->argument_context.Rip);
	TEST_CHECK_U64_EQ(frame, observed->argument_context.Rsp);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, observed->argument_context.Rbx);
	TEST_CHECK_U64_EQ(slot == 0 ? 0x2233445566778899ULL : 0x33445566778899aaULL, observed->argument_context.R12);
	const ULONGLONG *xmm6 = slot == 0 ? target_unwind_inner_xmm6 : target_unwind_outer_xmm6;
	TEST_CHECK(memcmp(xmm6, &observed->argument_context.Xmm6, sizeof(observed->argument_context.Xmm6)) == 0);
	printf("cleanup%u: flags=%lx error=%lu frame=%llx control-rva=%llx target-rva=%llx scope=%lu same-context=%u "
		   "argument-rip=%llx argument-rsp=%llx dispatcher-rip=%llx dispatcher-rsp=%llx rax=%llx rbx=%llx "
		   "r12=%llx xmm6=%llx:%llx\n",
		   observed->index, (unsigned long)observed->record.ExceptionFlags, (unsigned long)observed->error,
		   (unsigned long long)observed->frame, (unsigned long long)(dispatcher->ControlPc - image_base),
		   (unsigned long long)(dispatcher->TargetIp - image_base), (unsigned long)dispatcher->ScopeIndex,
		   (unsigned)(observed->argument_address == (ULONG_PTR)dispatcher->ContextRecord),
		   (unsigned long long)observed->argument_context.Rip, (unsigned long long)observed->argument_context.Rsp,
		   (unsigned long long)observed->dispatcher_context.Rip, (unsigned long long)observed->dispatcher_context.Rsp,
		   (unsigned long long)observed->argument_context.Rax, (unsigned long long)observed->argument_context.Rbx,
		   (unsigned long long)observed->argument_context.R12, (unsigned long long)observed->argument_context.Xmm6.High,
		   (unsigned long long)observed->argument_context.Xmm6.Low);
}

int main(int argc, char **argv) {
	change_scope_index = argc == 2 && strcmp(argv[1], "--scope-index") == 0;
	if (argc != 1 && !change_scope_index)
		return 2;
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "RtlUnwind");
	RtlUnwindFn unwind;
	_Static_assert(sizeof(exported) == sizeof(unwind), "Resolved unwind entry pointer width");
	memcpy(&unwind, &exported, sizeof(unwind));
	TEST_CHECK(unwind != NULL);
	ULONG_PTR image_base = (ULONG_PTR)GetModuleHandleA(NULL);
	TEST_CHECK(image_base != 0);
	target_unwind_record.ExceptionCode = 0xe0420a02;
	target_unwind_record.ExceptionAddress = (PVOID)target_unwind_inner_after_call;
	target_unwind_record.NumberParameters = 2;
	target_unwind_record.ExceptionInformation[0] = 0x0123456789abcdefULL;
	target_unwind_record.ExceptionInformation[1] = 0xfedcba9876543210ULL;
	SetLastError(0x4321);
	target_unwind_outer(unwind);
	DWORD resumed_error = GetLastError();
	TEST_CHECK_EQ(0x4321, resumed_error);
	TEST_CHECK_EQ(0, target_unwind_returned);
	TEST_CHECK_EQ(0, target_unwind_inner_returned);
	TEST_CHECK_EQ(1, target_unwind_landed);
	TEST_CHECK_EQ(2, handler_count);
	TEST_CHECK(!extra_handler);
	TEST_CHECK_EQ(change_scope_index ? 2 : 0, scope_changes);
	TEST_CHECK_EQ(EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND, target_unwind_record.ExceptionFlags);
	TEST_CHECK_U64_EQ(target_unwind_outer_rsp, target_unwind_landed_rsp);
	TEST_CHECK_U64_EQ(target_unwind_value, target_unwind_landed_rax);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, target_unwind_landed_rbx);
	TEST_CHECK_U64_EQ(0x33445566778899aaULL, target_unwind_landed_r12);
	TEST_CHECK(memcmp(target_unwind_outer_xmm6, target_unwind_landed_xmm6, sizeof(target_unwind_landed_xmm6)) == 0);
	for (unsigned index = 0; index < 2; ++index)
		check_observation(index, image_base);
	printf("landing: error=%lu flags=%lx rsp=%llx outer-rsp=%llx rax=%llx rbx=%llx r12=%llx xmm6=%llx:%llx "
		   "inner-control-rva=%llx outer-control-rva=%llx\n",
		   (unsigned long)resumed_error, (unsigned long)target_unwind_record.ExceptionFlags,
		   (unsigned long long)target_unwind_landed_rsp, (unsigned long long)target_unwind_outer_rsp,
		   (unsigned long long)target_unwind_landed_rax, (unsigned long long)target_unwind_landed_rbx,
		   (unsigned long long)target_unwind_landed_r12, (unsigned long long)target_unwind_landed_xmm6[1],
		   (unsigned long long)target_unwind_landed_xmm6[0],
		   (unsigned long long)((ULONG_PTR)target_unwind_inner_after_call - image_base),
		   (unsigned long long)((ULONG_PTR)target_unwind_outer_after_inner - image_base));
	printf("scope-index writes=%u\n", scope_changes);
	return 0;
}
