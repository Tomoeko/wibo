#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifndef _WIN64
#error This fixture requires the x64 exception ABI
#endif

#define CONSOLIDATE_CODE ((DWORD)0x80000029)
#define NESTED_CODE ((DWORD)0xe0420b09)

typedef VOID(WINAPI *CaptureFn)(PCONTEXT);
typedef VOID(__cdecl *RestoreFn)(PCONTEXT, PEXCEPTION_RECORD);
typedef VOID(WINAPI *UnwindFn)(PVOID, PVOID, PEXCEPTION_RECORD, PVOID, PCONTEXT, PUNWIND_HISTORY_TABLE);
typedef VOID(WINAPI *RaiseFn)(DWORD, DWORD, DWORD, const ULONG_PTR *);

_Static_assert(sizeof(CONTEXT) == 1232 && _Alignof(CONTEXT) == 16, "x64 context ABI");
_Static_assert(offsetof(CONTEXT, Rsp) == 152 && offsetof(CONTEXT, Rip) == 248, "x64 control ABI");
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "x64 record ABI");

CaptureFn capture_entry;
RestoreFn restore_entry;
UnwindFn unwind_entry;
RaiseFn raise_entry;
CONTEXT selected_context;
EXCEPTION_RECORD consolidation_record;
DWORD direct_mode, nested_mode;
ULONG_PTR outer_rsp, inner_rsp, callback_rsp, landed_rsp;
ULONG_PTR callback_rbx, callback_r12, landed_rax, landed_rbx, landed_r12;
ULONGLONG callback_xmm6[2], landed_xmm6[2];
DWORD callback_count, nested_returned, landed, unexpected_return, handler_errors;
DWORD inner_unwinds, outer_unwinds, inner_searches, outer_searches;
DWORD inner_flags, outer_flags, callback_error, final_error, trace;
ULONG_PTR callback_record_identity, callback_cookie, selected_rip_before_callback;
const ULONGLONG expected_xmm6[2]
	__attribute__((aligned(16))) = {UINT64_C(0x1020304050607080), UINT64_C(0x90a0b0c0d0e0f000)};
const ULONG_PTR expected_cookie = UINT64_C(0x31527496b8daf001);

extern VOID WINAPI consolidation_outer(VOID);
extern PVOID WINAPI consolidation_callback(PEXCEPTION_RECORD);
extern const BYTE consolidation_landing[], consolidation_decoy[], consolidation_after_inner[];

PVOID WINAPI consolidation_callback_body(PEXCEPTION_RECORD record) {
	++callback_count;
	trace = trace * 10 + 3;
	callback_record_identity = record == &consolidation_record;
	callback_error = GetLastError();
	callback_cookie = *(const ULONG_PTR *)(outer_rsp + 32);
	selected_rip_before_callback = selected_context.Rip;
	if (record != &consolidation_record || record->NumberParameters != 2 ||
		record->ExceptionInformation[1] != expected_cookie)
		++handler_errors;
	if (nested_mode) {
		raise_entry(NESTED_CODE, 0, 0, NULL);
		++nested_returned;
		trace = trace * 10 + 4;
	}
	return (PVOID)consolidation_landing;
}

EXCEPTION_DISPOSITION NTAPI consolidation_inner_handler(PEXCEPTION_RECORD record, PVOID frame, PCONTEXT context,
														PVOID dispatcher) {
	(void)context;
	(void)dispatcher;
	if (!record || (ULONG_PTR)frame != inner_rsp) {
		++handler_errors;
		return ExceptionContinueSearch;
	}
	if (record->ExceptionCode == CONSOLIDATE_CODE && (record->ExceptionFlags & EXCEPTION_UNWINDING)) {
		++inner_unwinds;
		trace = trace * 10 + 1;
		inner_flags = record->ExceptionFlags;
	} else if (record->ExceptionCode == NESTED_CODE) {
		++inner_searches;
	} else {
		++handler_errors;
	}
	return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION NTAPI consolidation_outer_handler(PEXCEPTION_RECORD record, PVOID frame, PCONTEXT context,
														PVOID dispatcher) {
	(void)context;
	(void)dispatcher;
	if (!record || (ULONG_PTR)frame != outer_rsp) {
		++handler_errors;
		return ExceptionContinueSearch;
	}
	if (record->ExceptionCode == CONSOLIDATE_CODE && (record->ExceptionFlags & EXCEPTION_UNWINDING)) {
		++outer_unwinds;
		trace = trace * 10 + 2;
		outer_flags = record->ExceptionFlags;
	} else if (record->ExceptionCode == NESTED_CODE && !(record->ExceptionFlags & EXCEPTION_UNWINDING)) {
		++outer_searches;
		return ExceptionContinueExecution;
	} else {
		++handler_errors;
	}
	return ExceptionContinueSearch;
}

static int run(DWORD direct, DWORD nested) {
	direct_mode = direct;
	nested_mode = nested;
	outer_rsp = inner_rsp = callback_rsp = landed_rsp = 0;
	callback_rbx = callback_r12 = landed_rax = landed_rbx = landed_r12 = 0;
	callback_count = nested_returned = landed = unexpected_return = handler_errors = 0;
	inner_unwinds = outer_unwinds = inner_searches = outer_searches = 0;
	inner_flags = outer_flags = callback_error = final_error = trace = 0;
	callback_record_identity = callback_cookie = selected_rip_before_callback = 0;
	memset(&selected_context, 0, sizeof(selected_context));
	memset(&consolidation_record, 0, sizeof(consolidation_record));
	consolidation_record.ExceptionCode = CONSOLIDATE_CODE;
	consolidation_record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
	consolidation_record.ExceptionAddress = (PVOID)consolidation_after_inner;
	consolidation_record.NumberParameters = 2;
	consolidation_record.ExceptionInformation[0] = (ULONG_PTR)consolidation_callback;
	consolidation_record.ExceptionInformation[1] = expected_cookie;
	SetLastError(0x4321);
	consolidation_outer();
	final_error = GetLastError();
	const int common_good = callback_count == 1 && callback_record_identity && callback_cookie == expected_cookie &&
							!handler_errors && !unexpected_return && landed == 1 && landed_rsp == outer_rsp &&
							landed_rbx == UINT64_C(0x1122334455667788) && landed_r12 == UINT64_C(0x33445566778899aa) &&
							landed_rax == UINT64_C(0x123456789abcdef0) &&
							memcmp(landed_xmm6, expected_xmm6, sizeof(expected_xmm6)) == 0;
	const int cleanup_good =
		direct ? (inner_unwinds == 0 && outer_unwinds == 0 &&
				  consolidation_record.ExceptionFlags == EXCEPTION_NONCONTINUABLE)
			   : (inner_unwinds == 1 && outer_unwinds == 1 &&
				  inner_flags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING) &&
				  outer_flags == (EXCEPTION_NONCONTINUABLE | EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND) &&
				  consolidation_record.ExceptionFlags == outer_flags);
	const int nested_good = inner_searches == 0 && outer_searches == nested && nested_returned == nested;
	printf("mode=%s nested=%lu callback=%lu record=%llu cookie=%llu landed=%lu returned=%lu errors=%lu "
		   "inner-unwinds=%lu outer-unwinds=%lu inner-flags=%lx outer-flags=%lx final-flags=%lx "
		   "inner-searches=%lu outer-searches=%lu nested-returned=%lu callback-error=%lu final-error=%lu "
		   "callback-rsp=%llx outer-rsp=%llx landed-rsp=%llx source-rip=%llx selected-rip=%llx "
		   "callback-rbx=%llx callback-r12=%llx landed-rax=%llx common=%d cleanup=%d nested-good=%d\n",
		   direct ? "restore" : "unwind", (unsigned long)nested, (unsigned long)callback_count,
		   (unsigned long long)callback_record_identity, (unsigned long long)(callback_cookie == expected_cookie),
		   (unsigned long)landed, (unsigned long)unexpected_return, (unsigned long)handler_errors,
		   (unsigned long)inner_unwinds, (unsigned long)outer_unwinds, (unsigned long)inner_flags,
		   (unsigned long)outer_flags, (unsigned long)consolidation_record.ExceptionFlags,
		   (unsigned long)inner_searches, (unsigned long)outer_searches, (unsigned long)nested_returned,
		   (unsigned long)callback_error, (unsigned long)final_error, (unsigned long long)callback_rsp,
		   (unsigned long long)outer_rsp, (unsigned long long)landed_rsp,
		   (unsigned long long)selected_rip_before_callback, (unsigned long long)selected_context.Rip,
		   (unsigned long long)callback_rbx, (unsigned long long)callback_r12, (unsigned long long)landed_rax,
		   common_good, cleanup_good, nested_good);
	fflush(stdout);
	printf("trace=%lu callback-xmm6-matches-target=%d landed-xmm6-matches-target=%d landed-rbx=%llx landed-r12=%llx\n",
		   (unsigned long)trace, memcmp(callback_xmm6, expected_xmm6, sizeof(expected_xmm6)) == 0,
		   memcmp(landed_xmm6, expected_xmm6, sizeof(expected_xmm6)) == 0, (unsigned long long)landed_rbx,
		   (unsigned long long)landed_r12);
	const DWORD expected_trace = direct ? (nested ? 345 : 35) : (nested ? 12345 : 1235);
	return !(common_good && cleanup_good && nested_good && trace == expected_trace && callback_error == 0x4321 &&
			 final_error == 0x4321 && selected_context.Rip == (ULONG_PTR)consolidation_landing);
}

int main(int argc, char **argv) {
	HMODULE native = GetModuleHandleA("ntdll.dll");
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	FARPROC entry = GetProcAddress(native, "RtlCaptureContext");
	memcpy(&capture_entry, &entry, sizeof(entry));
	entry = GetProcAddress(native, "RtlRestoreContext");
	memcpy(&restore_entry, &entry, sizeof(entry));
	entry = GetProcAddress(kernel, "RtlUnwindEx");
	memcpy(&unwind_entry, &entry, sizeof(entry));
	entry = GetProcAddress(kernel, "RaiseException");
	memcpy(&raise_entry, &entry, sizeof(entry));
	if (!capture_entry || !restore_entry || !unwind_entry || !raise_entry)
		return 2;
	int failed = 0;
	for (DWORD direct = 0; direct < 2; ++direct) {
		for (DWORD nested = 0; nested < 2; ++nested) {
			if (argc == 2) {
				const char *name =
					direct ? (nested ? "restore-nested" : "restore") : (nested ? "unwind-nested" : "unwind");
				if (strcmp(argv[1], name) != 0)
					continue;
			}
			failed |= run(direct, nested);
		}
	}
	return failed;
}
