#include <windows.h>

#include "test_assert.h"

#include <stddef.h>

#ifndef _WIN64
#error This fixture requires the x64 exception ABI
#endif

typedef VOID(WINAPI *RtlUnwindFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID);
typedef VOID(WINAPI *RtlUnwindExFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID, PCONTEXT, PUNWIND_HISTORY_TABLE);

_Static_assert(sizeof(CONTEXT) == 1232 && _Alignof(CONTEXT) == 16, "x64 context layout");
_Static_assert(offsetof(CONTEXT, FltSave) == 256 && offsetof(CONTEXT, VectorRegister) == 768, "legacy FP offsets");
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "x64 exception record width");
_Static_assert(sizeof(DISPATCHER_CONTEXT) == 80 && _Alignof(DISPATCHER_CONTEXT) == 8, "x64 dispatcher layout");
_Static_assert(offsetof(DISPATCHER_CONTEXT, ContextRecord) == 40, "context pointer offset");
_Static_assert(offsetof(DISPATCHER_CONTEXT, HistoryTable) == 64, "history pointer offset");
_Static_assert(sizeof(UNWIND_HISTORY_TABLE) == 216 && _Alignof(UNWIND_HISTORY_TABLE) == 8, "x64 history layout");
_Static_assert(offsetof(UNWIND_HISTORY_TABLE, Count) == 0, "history count offset");
_Static_assert(offsetof(UNWIND_HISTORY_TABLE, LowAddress) == 8, "history low offset");
_Static_assert(offsetof(UNWIND_HISTORY_TABLE, HighAddress) == 16, "history high offset");
_Static_assert(offsetof(UNWIND_HISTORY_TABLE, Entry) == 24, "history entries offset");
_Static_assert(sizeof(((UNWIND_HISTORY_TABLE *)0)->Entry) == 12 * 16, "history entry storage");

extern VOID WINAPI target_unwind_outer(RtlUnwindFn unwind);
extern VOID WINAPI target_unwind_ex_adapter(PVOID, PVOID, PEXCEPTION_RECORD, PVOID);
extern const BYTE target_unwind_inner_after_call[], target_unwind_outer_after_inner[], target_unwind_landing[];

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

RtlUnwindExFn target_unwind_ex_entry;
PCONTEXT target_unwind_ex_context;
PUNWIND_HISTORY_TABLE target_unwind_ex_history;

struct GuardedContext {
	BYTE before[16];
	CONTEXT context;
	BYTE after[16];
};

struct GuardedHistory {
	BYTE before[16];
	UNWIND_HISTORY_TABLE history;
	BYTE after[16];
};

static struct GuardedContext guarded_context;
static struct GuardedHistory guarded_history;

struct Observation {
	unsigned index;
	DWORD flags;
	DWORD error;
	ULONG_PTR frame;
	PCONTEXT argument;
	DISPATCHER_CONTEXT dispatcher;
	CONTEXT context;
};

static struct Observation observations[2];
static unsigned handler_count;
static BOOL bad_handler;

static void observe(unsigned index, EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context, PVOID dispatcher_pointer) {
	unsigned slot = handler_count++;
	if (slot >= 2 || !record || !context || !dispatcher_pointer || record != &target_unwind_record) {
		bad_handler = TRUE;
		return;
	}
	struct Observation *observation = &observations[slot];
	observation->index = index;
	observation->flags = record->ExceptionFlags;
	observation->error = GetLastError();
	observation->frame = (ULONG_PTR)frame;
	observation->argument = context;
	observation->dispatcher = *(DISPATCHER_CONTEXT *)dispatcher_pointer;
	observation->context = *context;
}

EXCEPTION_DISPOSITION NTAPI target_unwind_inner_handler(EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context,
														PVOID dispatcher) {
	observe(1, record, frame, context, dispatcher);
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION NTAPI target_unwind_outer_handler(EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context,
														PVOID dispatcher) {
	observe(2, record, frame, context, dispatcher);
	return ExceptionContinueSearch;
}

static BOOL filled_with(const void *bytes, size_t length, BYTE value) {
	const BYTE *data = bytes;
	for (size_t index = 0; index < length; ++index) {
		if (data[index] != value)
			return FALSE;
	}
	return TRUE;
}

static void log_context_groups(BOOL with_history, unsigned stage, const CONTEXT *context) {
	BOOL home_untouched = filled_with(context, offsetof(CONTEXT, ContextFlags), 0xa5);
	BOOL debug_untouched = filled_with(&context->Dr0, offsetof(CONTEXT, Rax) - offsetof(CONTEXT, Dr0), 0xa5);
	BOOL fp_tail_untouched = filled_with((const BYTE *)&context->FltSave + 416, 96, 0xa5);
	BOOL vector_untouched =
		filled_with(&context->VectorRegister, sizeof(CONTEXT) - offsetof(CONTEXT, VectorRegister), 0xa5);
	printf("history=%u stage=%u home-untouched=%u debug-untouched=%u fp-tail-untouched=%u vector-untouched=%u "
		   "dr0=%llx dr1=%llx dr2=%llx dr3=%llx dr6=%llx dr7=%llx rbx=%llx r12=%llx xmm6=%llx:%llx\n",
		   (unsigned)with_history, stage, (unsigned)home_untouched, (unsigned)debug_untouched,
		   (unsigned)fp_tail_untouched, (unsigned)vector_untouched, (unsigned long long)context->Dr0,
		   (unsigned long long)context->Dr1, (unsigned long long)context->Dr2, (unsigned long long)context->Dr3,
		   (unsigned long long)context->Dr6, (unsigned long long)context->Dr7, (unsigned long long)context->Rbx,
		   (unsigned long long)context->R12, (unsigned long long)context->Xmm6.High,
		   (unsigned long long)context->Xmm6.Low);
}

static void run(BOOL with_history) {
	memset(&guarded_context, 0xa5, sizeof(guarded_context));
	memset(&guarded_history, 0xa5, sizeof(guarded_history));
	memset(&guarded_history.history, 0, sizeof(guarded_history.history));
	memset(&target_unwind_record, 0, sizeof(target_unwind_record));
	memset(observations, 0, sizeof(observations));
	handler_count = 0;
	bad_handler = FALSE;
	target_unwind_returned = target_unwind_inner_returned = target_unwind_landed = 0;
	target_unwind_ex_context = &guarded_context.context;
	target_unwind_ex_history = with_history ? &guarded_history.history : NULL;
	target_unwind_record.ExceptionCode = 0xe0420a04;
	target_unwind_record.ExceptionAddress = (PVOID)target_unwind_inner_after_call;
	SetLastError(0x4321);
	target_unwind_outer(target_unwind_ex_adapter);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(0x4321, error);
	TEST_CHECK_EQ(0, target_unwind_returned);
	TEST_CHECK_EQ(0, target_unwind_inner_returned);
	TEST_CHECK_EQ(1, target_unwind_landed);
	TEST_CHECK_EQ(2, handler_count);
	TEST_CHECK(!bad_handler);
	TEST_CHECK(filled_with(guarded_context.before, sizeof(guarded_context.before), 0xa5));
	TEST_CHECK(filled_with(guarded_context.after, sizeof(guarded_context.after), 0xa5));
	TEST_CHECK(filled_with(guarded_history.before, sizeof(guarded_history.before), 0xa5));
	TEST_CHECK(filled_with(guarded_history.after, sizeof(guarded_history.after), 0xa5));
	for (unsigned slot = 0; slot < 2; ++slot) {
		const struct Observation *observation = &observations[slot];
		TEST_CHECK_EQ(slot + 1, observation->index);
		TEST_CHECK(observation->argument == target_unwind_ex_context);
		TEST_CHECK(observation->dispatcher.ContextRecord == target_unwind_ex_context);
		TEST_CHECK(observation->dispatcher.HistoryTable == target_unwind_ex_history);
		TEST_CHECK_EQ(EXCEPTION_UNWINDING | (slot ? EXCEPTION_TARGET_UNWIND : 0), observation->flags);
		TEST_CHECK_EQ(0x4321, observation->error);
		TEST_CHECK_EQ(CONTEXT_FULL | CONTEXT_SEGMENTS, observation->context.ContextFlags);
		ULONG_PTR frame = slot == 0 ? target_unwind_inner_rsp : target_unwind_outer_rsp;
		ULONG_PTR control =
			slot == 0 ? (ULONG_PTR)target_unwind_inner_after_call : (ULONG_PTR)target_unwind_outer_after_inner;
		TEST_CHECK_U64_EQ(frame, observation->frame);
		TEST_CHECK_U64_EQ(frame, observation->dispatcher.EstablisherFrame);
		TEST_CHECK_U64_EQ(frame, observation->context.Rsp);
		TEST_CHECK_U64_EQ(control, observation->context.Rip);
		TEST_CHECK_U64_EQ(control, observation->dispatcher.ControlPc);
		TEST_CHECK_U64_EQ((ULONG_PTR)target_unwind_landing, observation->dispatcher.TargetIp);
		TEST_CHECK_EQ(0, observation->dispatcher.ScopeIndex);
		TEST_CHECK_U64_EQ(0x1122334455667788ULL, observation->context.Rbx);
		TEST_CHECK_U64_EQ(slot == 0 ? 0x2233445566778899ULL : 0x33445566778899aaULL, observation->context.R12);
		const ULONGLONG *xmm6 = slot == 0 ? target_unwind_inner_xmm6 : target_unwind_outer_xmm6;
		TEST_CHECK(memcmp(xmm6, &observation->context.Xmm6, sizeof(observation->context.Xmm6)) == 0);
		printf("history=%u cleanup%u flags=%lx error=%lu context-flags=%lx frame=%llx rsp=%llx rip=%llx "
			   "control=%llx target=%llx caller-context=1 history-pointer=1\n",
			   (unsigned)with_history, observation->index, (unsigned long)observation->flags,
			   (unsigned long)observation->error, (unsigned long)observation->context.ContextFlags,
			   (unsigned long long)observation->frame, (unsigned long long)observation->context.Rsp,
			   (unsigned long long)observation->context.Rip, (unsigned long long)observation->dispatcher.ControlPc,
			   (unsigned long long)observation->dispatcher.TargetIp);
		log_context_groups(with_history, slot + 1, &observation->context);
	}
	const CONTEXT *context = &guarded_context.context;
	TEST_CHECK_EQ(EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND, target_unwind_record.ExceptionFlags);
	TEST_CHECK_EQ(CONTEXT_FULL | CONTEXT_SEGMENTS, context->ContextFlags);
	TEST_CHECK_U64_EQ(target_unwind_outer_rsp, context->Rsp);
	TEST_CHECK_U64_EQ((ULONG_PTR)target_unwind_landing, context->Rip);
	TEST_CHECK_U64_EQ(target_unwind_value, context->Rax);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, context->Rbx);
	TEST_CHECK_U64_EQ(0x33445566778899aaULL, context->R12);
	TEST_CHECK(memcmp(target_unwind_outer_xmm6, &context->Xmm6, sizeof(context->Xmm6)) == 0);
	TEST_CHECK_U64_EQ(context->Rsp, target_unwind_landed_rsp);
	TEST_CHECK_U64_EQ(context->Rax, target_unwind_landed_rax);
	TEST_CHECK_U64_EQ(context->Rbx, target_unwind_landed_rbx);
	TEST_CHECK_U64_EQ(context->R12, target_unwind_landed_r12);
	TEST_CHECK(memcmp(&context->Xmm6, target_unwind_landed_xmm6, sizeof(target_unwind_landed_xmm6)) == 0);
	BOOL history_unchanged = filled_with(&guarded_history.history, sizeof(guarded_history.history), 0);
	printf("history=%u final error=%lu flags=%lx rsp=%llx outer-rsp=%llx rip=%llx landing=%llx rax=%llx "
		   "landed-rax=%llx history-count=%lu history-unchanged=%u\n",
		   (unsigned)with_history, (unsigned long)error, (unsigned long)context->ContextFlags,
		   (unsigned long long)context->Rsp, (unsigned long long)target_unwind_outer_rsp,
		   (unsigned long long)context->Rip, (unsigned long long)(ULONG_PTR)target_unwind_landing,
		   (unsigned long long)context->Rax, (unsigned long long)target_unwind_landed_rax,
		   (unsigned long)guarded_history.history.Count, (unsigned)history_unchanged);
	log_context_groups(with_history, 3, context);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "RtlUnwindEx");
	_Static_assert(sizeof(exported) == sizeof(target_unwind_ex_entry), "Resolved unwind entry pointer width");
	memcpy(&target_unwind_ex_entry, &exported, sizeof(target_unwind_ex_entry));
	TEST_CHECK(target_unwind_ex_entry != NULL);
	run(FALSE);
	run(TRUE);
	return 0;
}
