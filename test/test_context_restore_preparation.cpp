#include "context_restore.h"
#include "heap.h"
#include "setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
CONTEXT64 selected;
EXCEPTION_RECORD record;
unsigned transferCalls;

bool contains(const void *pointer, const void *base, size_t size) {
	const auto address = reinterpret_cast<ULONGLONG>(pointer);
	const auto start = reinterpret_cast<ULONGLONG>(base);
	return address >= start && address - start < size;
}
} // namespace

namespace wibo::heap {
VmStatus virtualQuery(const void *address, MEMORY_BASIC_INFORMATION *information) {
	// The isolated fixture exposes only its two owned, readable objects.
	const void *base = contains(address, &selected, sizeof(selected)) ? static_cast<const void *>(&selected)
					   : contains(address, &record, sizeof(record))	  ? static_cast<const void *>(&record)
																	  : nullptr;
	if (!base)
		return VmStatus::InvalidParameter;
	*information = {};
	information->BaseAddress = reinterpret_cast<ULONGLONG>(base);
	information->RegionSize = base == &selected ? sizeof(selected) : sizeof(record);
	information->State = MEM_COMMIT;
	information->Protect = PAGE_READWRITE;
	return VmStatus::Success;
}
} // namespace wibo::heap

namespace wibo {
bool hasActiveVectoredExceptionTraversal() noexcept { return false; }
bool linkSoftwareExceptionActivation(SoftwareExceptionActivation64 *, const CONTEXT64 *, ULONGLONG, ULONGLONG,
									 SoftwareExceptionActivationPhase64) {
	++transferCalls;
	return false;
}
bool unlinkSoftwareExceptionActivation(SoftwareExceptionActivation64 *) {
	++transferCalls;
	return false;
}
} // namespace wibo

bool wiboValidateContextRestoreTransfer64(ULONGLONG) {
	++transferCalls;
	return false;
}
bool wiboPrepareContextRestoreTransfer64(ULONGLONG) {
	++transferCalls;
	return false;
}
extern "C" {
thread_local TEB *currentThreadTeb = nullptr;
}
#ifdef __APPLE__
TEB *currentTebForGuestTransition() { return currentThreadTeb; }
#endif
namespace kernel32 {
[[noreturn]] void exitInternal(DWORD code) { std::exit(static_cast<int>(code)); }
} // namespace kernel32

int main() {
	constexpr struct {
		DWORD code, flags, count;
	} cases[] = {
		{0x80000026, 0, 1}, {0x80000029, 0, 0}, {0x80000029, 0, 16}, {0x80000029, 0x40, 1}, {0xe0420b10, 0, 1}};
	unsigned failures = 0;
	for (const auto &item : cases) {
		selected = {};
		record = {};
		record.ExceptionCode = item.code;
		record.ExceptionFlags = item.flags;
		record.NumberParameters = item.count;
		SoftwareExceptionCapture64 capture{};
		capture.context.Rcx = reinterpret_cast<ULONGLONG>(&selected);
		capture.context.Rdx = reinterpret_cast<ULONGLONG>(&record);
		SoftwareExceptionCapture64 beforeCapture{};
		CONTEXT64 beforeContext{};
		EXCEPTION_RECORD beforeRecord{};
		std::memcpy(&beforeCapture, &capture, sizeof(capture));
		std::memcpy(&beforeContext, &selected, sizeof(selected));
		std::memcpy(&beforeRecord, &record, sizeof(record));
		ContextRestorePreparation64 output{};
		const auto disposition = wiboPrepareContextRestore64(&capture, &output);
		const bool good = disposition == static_cast<DWORD>(ContextRestoreKind64::UnsupportedExceptionRecord) &&
						  output.kind == ContextRestoreKind64::UnsupportedExceptionRecord &&
						  output.status == 0xc00000bb && transferCalls == 0 &&
						  std::memcmp(&capture, &beforeCapture, sizeof(capture)) == 0 &&
						  std::memcmp(&selected, &beforeContext, sizeof(selected)) == 0 &&
						  std::memcmp(&record, &beforeRecord, sizeof(record)) == 0;
		std::printf("code=%08x flags=%x count=%u disposition=%u unchanged=%u\n", item.code, item.flags, item.count,
					disposition, good);
		failures += !good;
	}
	return failures ? 1 : 0;
}
