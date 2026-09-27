#include "software_exception_dispatch.h"

#include "common.h"
#include "kernel32/internal.h"

#include <cstdio>

#if defined(__APPLE__) && defined(WIBO_GUEST_64)
#include <array>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#ifdef WIBO_GUEST_64
[[noreturn]] void wiboUnsupportedSoftwareExceptionDispatch64(const SoftwareExceptionCapture64 *capture,
															 const SoftwareExceptionDecision64 *decision) {
	// The dispatcher has returned from every callback and entered host context.
	// General frame handling and secondary exception dispatch are unavailable.
	std::fprintf(stderr, "Unsupported software exception dispatch: kind=%u code=0x%08x status=0x%08x\n",
				 static_cast<unsigned>(decision->kind), decision->originalCode, decision->failureCode);
	if (wibo::debugEnabled && capture && capture->record) {
		const auto &context = capture->callerCapture ? capture->callerCapture->context : capture->context;
		DEBUG_LOG("Software exception context: rip=%llx rsp=%llx rbp=%llx rbx=%llx rsi=%llx rdi=%llx "
				  "r12=%llx r13=%llx r14=%llx r15=%llx\n",
				  context.Rip, context.Rsp, context.Rbp, context.Rbx, context.Rsi, context.Rdi, context.R12,
				  context.R13, context.R14, context.R15);
		const auto &record = *capture->record;
		DEBUG_LOG("Software exception record: flags=0x%08x address=%llx count=%u\n", record.ExceptionFlags,
				  record.ExceptionAddress, record.NumberParameters);
		for (DWORD index = 0; index < record.NumberParameters && index < EXCEPTION_MAXIMUM_PARAMETERS; ++index) {
			DEBUG_LOG("Software exception argument[%u]=%llx\n", index, record.ExceptionInformation[index]);
		}
#if defined(__APPLE__)
		// Read through the kernel so diagnostic inspection cannot fault on an inaccessible stack.
		std::array<ULONGLONG, 64> words{};
		mach_vm_size_t bytesRead = 0;
		const auto status = mach_vm_read_overwrite(mach_task_self(), context.Rsp, sizeof(words),
												   reinterpret_cast<mach_vm_address_t>(words.data()), &bytesRead);
		if (status == KERN_SUCCESS) {
			for (size_t index = 0; index < words.size() && index < bytesRead / sizeof(words[0]); ++index) {
				DEBUG_LOG("Software exception stack[%zu]=%llx\n", index, words[index]);
			}
		}
#endif
	}
	kernel32::exitInternal(decision->failureCode);
}
#endif
