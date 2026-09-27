#include <windows.h>

#include "test_assert.h"

typedef PVOID(WINAPI *unwind_fn)(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID *, PDWORD64,
								 PKNONVOLATILE_CONTEXT_POINTERS);
static unwind_fn unwind;
static BYTE *image;
static RUNTIME_FUNCTION entry = {16, 128, 256};
static DWORD64 stack[64];
static const DWORD64 returnAddress = 0x123456789abcdef0;
static CONTEXT context;
static KNONVOLATILE_CONTEXT_POINTERS pointers;
static DWORD64 frame;
static PVOID handlerData;

static void prepare(const BYTE *info, size_t length) {
	memset(image, 0x90, 4096);
	memcpy(image + entry.UnwindData, info, length);
	memset(stack, 0, sizeof(stack));
	memset(&context, 0, sizeof(context));
	memset(&pointers, 0, sizeof(pointers));
	context.ContextFlags = CONTEXT_ALL;
	context.Rsp = (DWORD64)(ULONG_PTR)stack;
	frame = 0;
	handlerData = (PVOID)(ULONG_PTR)0xdead;
}

static PVOID step(DWORD pc, DWORD kind) {
	SetLastError(0x20001234);
	PVOID handler = unwind(kind, (ULONG_PTR)image, (ULONG_PTR)image + entry.BeginAddress + pc, &entry, &context,
						   &handlerData, &frame, &pointers);
	TEST_CHECK_EQ(0x20001234, GetLastError());
	return handler;
}

static void expect_return(unsigned slot) {
	TEST_CHECK_U64_EQ(returnAddress, context.Rip);
	TEST_CHECK_U64_EQ((ULONG_PTR)&stack[slot + 1], context.Rsp);
}

static void test_allocations_and_prologue(void) {
	const BYTE small[] = {1, 5, 2, 0, 5, 0x32, 1, 0x30};
	prepare(small, sizeof(small));
	stack[4] = 0x76543210;
	stack[5] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(5);
	TEST_CHECK_U64_EQ(stack[4], context.Rbx);
	TEST_CHECK(pointers.IntegerContext[3] == &stack[4]);
	TEST_CHECK_U64_EQ((ULONG_PTR)stack, frame);

	prepare(small, sizeof(small));
	stack[0] = 0x98765432;
	stack[1] = returnAddress;
	TEST_CHECK(step(3, UNW_FLAG_EHANDLER) == NULL);
	expect_return(1);
	TEST_CHECK_U64_EQ(stack[0], context.Rbx);

	prepare(small, sizeof(small));
	stack[0] = returnAddress;
	TEST_CHECK(step(0, 0) == NULL);
	expect_return(0);

	const BYTE large[] = {1, 7, 2, 0, 7, 1, 24, 0};
	prepare(large, sizeof(large));
	stack[24] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(24);

	const BYTE unscaled[] = {1, 7, 3, 0, 7, 0x11, 0xa0, 0, 0, 0, 0, 0};
	prepare(unscaled, sizeof(unscaled));
	stack[20] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(20);
}

static void test_saved_registers_and_frame(void) {
	const BYTE saved[] = {1, 12, 5, 0, 12, 0x68, 1, 0, 8, 0xc4, 3, 0, 4, 0x52, 0, 0};
	prepare(saved, sizeof(saved));
	const M128A vector = {0x12345678, 0x76543210};
	memcpy(&stack[2], &vector, sizeof(vector));
	stack[3] = vector.High;
	stack[6] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(6);
	TEST_CHECK_U64_EQ(stack[3], context.R12);
	TEST_CHECK(pointers.IntegerContext[12] == &stack[3]);
	TEST_CHECK(memcmp(&context.Xmm6, &vector, sizeof(vector)) == 0);
	TEST_CHECK(pointers.FloatingContext[6] == (M128A *)&stack[2]);

	const BYTE framed[] = {1, 8, 3, 0x25, 8, 3, 5, 0x32, 1, 0x50, 0, 0};
	prepare(framed, sizeof(framed));
	context.Rbp = (ULONG_PTR)&stack[4];
	context.Rsp = (ULONG_PTR)&stack[0];
	stack[4] = 0x3333;
	stack[5] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(5);
	TEST_CHECK_U64_EQ((ULONG_PTR)&stack[0], frame);
	TEST_CHECK_U64_EQ(0x3333, context.Rbp);

	const BYTE reservedFrame[] = {1, 8, 3, 0x25, 8, 0xf3, 5, 0x32, 1, 0x50, 0, 0};
	prepare(reservedFrame, sizeof(reservedFrame));
	context.Rbp = (ULONG_PTR)&stack[4];
	context.Rsp = (ULONG_PTR)&stack[0];
	stack[4] = 0x4444;
	stack[5] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(5);
	TEST_CHECK_U64_EQ((ULONG_PTR)&stack[0], frame);
	TEST_CHECK_U64_EQ(0x4444, context.Rbp);
	TEST_CHECK(pointers.IntegerContext[5] == &stack[4]);
}

static void test_handlers_and_chains(void) {
	const BYTE handler[] = {
		1 | (UNW_FLAG_EHANDLER << 3), 4, 1, 0, 4, 0x12, 0, 0, 0x80, 0x01, 0, 0, 0x11, 0x22, 0x33, 0x44};
	prepare(handler, sizeof(handler));
	stack[2] = returnAddress;
	TEST_CHECK(step(32, UNW_FLAG_EHANDLER) == image + 384);
	expect_return(2);
	TEST_CHECK(handlerData == image + 268);

	prepare(handler, sizeof(handler));
	stack[2] = returnAddress;
	TEST_CHECK(step(32, UNW_FLAG_UHANDLER) == NULL);
	expect_return(2);

	const BYTE chained[] = {1 | (UNW_FLAG_CHAININFO << 3), 0, 0, 0, 16, 0, 0, 0, 128, 0, 0, 0, 0x20, 0x01, 0, 0};
	prepare(chained, sizeof(chained));
	const BYTE primary[] = {1, 4, 1, 0, 4, 0x22, 0, 0};
	memcpy(image + 288, primary, sizeof(primary));
	stack[3] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(3);
}

static void test_epilogues(void) {
	const BYTE info[] = {1, 5, 2, 0, 5, 0x32, 1, 0xc0};
	const BYTE code[] = {0x48, 0x83, 0xc4, 0x20, 0x41, 0x5c, 0xc3};
	for (unsigned phase = 0; phase < 3; ++phase) {
		prepare(info, sizeof(info));
		memcpy(image + 64, code, sizeof(code));
		const unsigned offsets[] = {0, 4, 6};
		context.Rsp = (ULONG_PTR)&stack[phase == 0 ? 0 : phase == 1 ? 4 : 5];
		stack[4] = 0xabcdef;
		stack[5] = returnAddress;
		TEST_CHECK(step(48 + offsets[phase], UNW_FLAG_EHANDLER) == NULL);
		expect_return(5);
		if (phase != 2)
			TEST_CHECK_U64_EQ(stack[4], context.R12);
	}
	const BYTE empty[] = {1, 0, 0, 0};
	const BYTE ret[] = {0xc2, 0x10, 0};
	prepare(empty, sizeof(empty));
	memcpy(image + 64, ret, sizeof(ret));
	stack[0] = returnAddress;
	TEST_CHECK(step(48, 0) == NULL);
	TEST_CHECK_U64_EQ(returnAddress, context.Rip);
	TEST_CHECK_U64_EQ((ULONG_PTR)&stack[1], context.Rsp);
}

static void test_version_two(void) {
	const BYTE endInfo[] = {2, 5, 4, 0, 3, 0x16, 0, 6, 5, 0x32, 1, 0xc0};
	const BYTE offsetInfo[] = {2, 5, 4, 0, 3, 6, 60, 6, 5, 0x32, 1, 0xc0};
	const BYTE code[] = {0x48, 0x83, 0xc4, 0x20, 0x41, 0x5c, 0xc3};
	for (unsigned mode = 0; mode < 2; ++mode) {
		for (unsigned phase = 0; phase < 4; ++phase) {
			prepare(mode ? offsetInfo : endInfo, sizeof(endInfo));
			unsigned start = mode ? 64 : 121;
			memcpy(image + start, code, sizeof(code));
			const unsigned positions[] = {32, start - 16, start - 12, start - 10};
			context.Rsp = (ULONG_PTR)&stack[phase < 2 ? 0 : phase == 2 ? 4 : 5];
			stack[4] = 0x11223344;
			stack[5] = returnAddress;
			TEST_CHECK(step(positions[phase], 0) == NULL);
			expect_return(5);
			if (phase != 3)
				TEST_CHECK_U64_EQ(stack[4], context.R12);
		}
	}
}

static void test_far_saves_and_machine_frame(void) {
	const BYTE saved[] = {1, 12, 7, 0, 12, 0x79, 32, 0, 0, 0, 8, 0xd5, 16, 0, 0, 0, 4, 0x72, 0, 0};
	prepare(saved, sizeof(saved));
	const M128A vector = {0x11223344, 0x55667788};
	stack[2] = 0x7777;
	memcpy(&stack[4], &vector, sizeof(vector));
	stack[8] = returnAddress;
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(8);
	TEST_CHECK_U64_EQ(stack[2], context.R13);
	TEST_CHECK(memcmp(&context.Xmm7, &vector, sizeof(vector)) == 0);
	TEST_CHECK(pointers.FloatingContext[7] == (M128A *)&stack[4]);
	for (unsigned error = 0; error < 2; ++error) {
		BYTE info[] = {1, 1, 1, 0, 1, 0x0a, 0, 0};
		info[5] |= error << 4;
		prepare(info, sizeof(info));
		stack[error] = returnAddress;
		stack[error + 3] = (ULONG_PTR)&stack[20];
		TEST_CHECK(step(32, 0) == NULL);
		TEST_CHECK_U64_EQ(returnAddress, context.Rip);
		TEST_CHECK_U64_EQ((ULONG_PTR)&stack[20], context.Rsp);
	}
}

static void test_combined_epilogue_scopes(void) {
	const BYTE info[] = {2, 2, 3, 0, 3, 0x16, 64, 6, 2, 0xc0, 0, 0};
	const BYTE code[] = {0x41, 0x5c, 0xc3};
	const unsigned positions[] = {64, 66, 125, 127};
	for (unsigned phase = 0; phase < 4; ++phase) {
		prepare(info, sizeof(info));
		memcpy(image + 64, code, sizeof(code));
		memcpy(image + 125, code, sizeof(code));
		stack[0] = 0x11223344;
		stack[1] = returnAddress;
		context.Rsp = (ULONG_PTR)&stack[phase & 1];
		context.R12 = stack[0];
		TEST_CHECK(step(positions[phase] - 16, 0) == NULL);
		expect_return(1);
		TEST_CHECK_U64_EQ(stack[0], context.R12);
	}
}

static void test_chained_body_jump(void) {
	typedef LONG(WINAPI * add_fn)(PVOID *, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
	typedef VOID(WINAPI * delete_fn)(PVOID);
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	add_fn add = (add_fn)(ULONG_PTR)GetProcAddress(module, "RtlAddGrowableFunctionTable");
	delete_fn remove = (delete_fn)(ULONG_PTR)GetProcAddress(module, "RtlDeleteGrowableFunctionTable");
	TEST_CHECK(add && remove);
	RUNTIME_FUNCTION entries[] = {{16, 64, 256}, {128, 192, 288}};
	entry = entries[1];
	const BYTE chained[] = {1 | (UNW_FLAG_CHAININFO << 3), 0, 0, 0};
	prepare(chained, sizeof(chained));
	memcpy(image + 292, &entries[0], sizeof(entries[0]));
	const BYTE primary[] = {1, 4, 1, 0, 4, 0x32, 0, 0};
	memcpy(image + 256, primary, sizeof(primary));
	const BYTE jump[] = {0xe9, 0x7b, 0xff, 0xff, 0xff};
	memcpy(image + 160, jump, sizeof(jump));
	stack[4] = returnAddress;
	PVOID table;
	TEST_CHECK_EQ(0, add(&table, entries, 2, 2, (ULONG_PTR)image, (ULONG_PTR)image + 4096));
	TEST_CHECK(step(32, 0) == NULL);
	expect_return(4);
	remove(table);
	entry = (RUNTIME_FUNCTION){16, 128, 256};
}

static void test_machine_frame_epilogue(void) {
	const BYTE info[] = {2, 1, 3, 0, 1, 6, 2, 6, 1, 0x0a, 0, 0};
	prepare(info, sizeof(info));
	image[126] = 0x48;
	image[127] = 0xcf;
	stack[0] = returnAddress;
	stack[3] = (ULONG_PTR)&stack[20];
	TEST_CHECK(step(110, 0) == NULL);
	TEST_CHECK_U64_EQ(returnAddress, context.Rip);
	TEST_CHECK_U64_EQ((ULONG_PTR)&stack[20], context.Rsp);
	// Some comparison runtimes lack version 2 machine-frame epilogue support.
	if (!getenv("WIBO_TEST_DESCRIBED_MACHINE_FRAME"))
		return;
	const BYTE combined[] = {2, 5, 6, 0, 5, 6, 64, 6, 5, 0xc0, 3, 0x30, 2, 2, 1, 0x1a};
	const BYTE code[] = {0x41, 0x5c, 0x5b, 0x59, 0x48, 0xcf};
	const unsigned positions[] = {0, 2, 3, 4};
	for (unsigned phase = 0; phase < 4; ++phase) {
		prepare(combined, sizeof(combined));
		memcpy(image + 64, code, sizeof(code));
		stack[0] = 0x11223344;
		stack[1] = 0x55667788;
		stack[3] = returnAddress;
		stack[6] = (ULONG_PTR)&stack[20];
		context.R12 = stack[0];
		context.Rbx = stack[1];
		context.Rsp = (ULONG_PTR)&stack[phase];
		TEST_CHECK(step(48 + positions[phase], 0) == NULL);
		TEST_CHECK_U64_EQ(returnAddress, context.Rip);
		TEST_CHECK_U64_EQ((ULONG_PTR)&stack[20], context.Rsp);
		TEST_CHECK_U64_EQ(stack[0], context.R12);
		TEST_CHECK_U64_EQ(stack[1], context.Rbx);
	}
}

static void test_tail_jumps(void) {
	const BYTE info[] = {1, 4, 1, 0, 4, 0x32, 0, 0};
	const BYTE tail[] = {0x48, 0x83, 0xc4, 0x20, 0xe9, 0, 1, 0, 0};
	prepare(info, sizeof(info));
	memcpy(image + 64, tail, sizeof(tail));
	stack[4] = returnAddress;
	TEST_CHECK(step(48, 0) == NULL);
	expect_return(4);
	prepare(info, sizeof(info));
	const BYTE internal[] = {0xe9, 0, 0, 0, 0};
	memcpy(image + 64, internal, sizeof(internal));
	stack[4] = returnAddress;
	TEST_CHECK(step(48, 0) == NULL);
	expect_return(4);
}

static void test_frame_epilogue(void) {
	const BYTE ordinary[] = {0x48, 0x8d, 0x65, 0x20, 0x5d, 0xc3};
	const BYTE indexed[] = {0x49, 0x8d, 0x64, 0x24, 0x20, 0x41, 0x5c, 0xc3};
	for (unsigned mode = 0; mode < 2; ++mode) {
		const BYTE reg = mode ? 12 : 5;
		BYTE info[] = {1 | (UNW_FLAG_EHANDLER << 3), 10, 3, 0x20, 10, 3, 6, 0x72, 2, 0, 0, 0};
		info[3] |= reg;
		info[9] = reg << 4;
		prepare(info, sizeof(info));
		DWORD handler = 384;
		memcpy(image + 268, &handler, sizeof(handler));
		memcpy(image + 64, mode ? indexed : ordinary, mode ? sizeof(indexed) : sizeof(ordinary));
		context.Rsp = (ULONG_PTR)&stack[1];
		if (mode)
			context.R12 = (ULONG_PTR)&stack[4];
		else
			context.Rbp = (ULONG_PTR)&stack[4];
		stack[8] = 0x4444;
		stack[9] = returnAddress;
		TEST_CHECK(step(48, UNW_FLAG_EHANDLER) == (mode ? image + 384 : NULL));
		expect_return(9);
		TEST_CHECK_U64_EQ(0x4444, mode ? context.R12 : context.Rbp);
	}
}

static unwind_fn resolve_unwind(LPCWSTR moduleName) {
	HMODULE module = GetModuleHandleW(moduleName);
	TEST_CHECK(module != NULL);
	FARPROC address = GetProcAddress(module, "RtlVirtualUnwind");
	unwind_fn function = NULL;
	_Static_assert(sizeof(address) == sizeof(function), "function pointer size");
	memcpy(&function, &address, sizeof(function));
	TEST_CHECK(function != NULL);
	return function;
}

int main(void) {
	unwind = resolve_unwind(L"ntdll.dll");
	image = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(image != NULL);
	test_allocations_and_prologue();
	test_saved_registers_and_frame();
	test_handlers_and_chains();
	test_epilogues();
	test_version_two();
	test_far_saves_and_machine_frame();
	test_combined_epilogue_scopes();
	test_chained_body_jump();
	test_machine_frame_epilogue();
	test_tail_jumps();
	test_frame_epilogue();
	unwind = resolve_unwind(L"kernel32.dll");
	test_allocations_and_prologue();
	test_saved_registers_and_frame();
	test_handlers_and_chains();
	test_epilogues();
	TEST_CHECK(VirtualFree(image, 0, MEM_RELEASE));
	return 0;
}
