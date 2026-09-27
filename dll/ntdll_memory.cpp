#include "ntdll.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "kernel32/memoryapi.h"
#include "kernel32/processthreadsapi.h"

#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace {

constexpr ULONG kMemoryBasicInformation = 0;
constexpr ULONG kMemoryMappedFilenameInformation = 2;
constexpr ULONG kMemoryRegionInformation = 3;
constexpr ULONG kMemoryWorkingSetExInformation = 4;
constexpr ULONG kMemoryImageInformation = 6;
constexpr ULONG kHostRuntimeInformation = 1000;
constexpr ULONG kHostRuntimeGuest32Information = 1001;

#ifdef WIBO_GUEST_64
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 48);
static_assert(offsetof(MEMORY_BASIC_INFORMATION, RegionSize) == 24);
#else
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 28);
static_assert(offsetof(MEMORY_BASIC_INFORMATION, RegionSize) == 12);
#endif

void copyBasicInformation(PVOID output, const MEMORY_BASIC_INFORMATION &information) {
	// Only the named fields are written; padding belongs to the caller's buffer.
	auto write = [output](size_t offset, const auto &member) {
		std::memcpy(static_cast<BYTE *>(output) + offset, &member, sizeof(member));
	};
	write(offsetof(MEMORY_BASIC_INFORMATION, BaseAddress), information.BaseAddress);
	write(offsetof(MEMORY_BASIC_INFORMATION, AllocationBase), information.AllocationBase);
	write(offsetof(MEMORY_BASIC_INFORMATION, AllocationProtect), information.AllocationProtect);
	write(offsetof(MEMORY_BASIC_INFORMATION, RegionSize), information.RegionSize);
	write(offsetof(MEMORY_BASIC_INFORMATION, State), information.State);
	write(offsetof(MEMORY_BASIC_INFORMATION, Protect), information.Protect);
	write(offsetof(MEMORY_BASIC_INFORMATION, Type), information.Type);
}

} // namespace

NTSTATUS WINAPI ntdll::NtQueryVirtualMemory(HANDLE processHandle, LPCVOID baseAddress, ULONG informationClass,
											PVOID information, SIZE_T length, PSIZE_T returnedLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryVirtualMemory(%p, %p, %u, %p, %llu, %p)\n", processHandle, baseAddress, informationClass,
			  information, static_cast<unsigned long long>(length), returnedLength);
	if (informationClass != kMemoryBasicInformation) {
		switch (informationClass) {
		case kMemoryMappedFilenameInformation:
		case kMemoryRegionInformation:
		case kMemoryWorkingSetExInformation:
		case kMemoryImageInformation:
		case kHostRuntimeInformation:
		case kHostRuntimeGuest32Information:
			// These classes require information or host services not represented here.
			return STATUS_NOT_SUPPORTED;
		default:
			return STATUS_INVALID_INFO_CLASS;
		}
	}
	if (length < sizeof(MEMORY_BASIC_INFORMATION)) {
#ifndef WIBO_GUEST_64
		if (returnedLength)
			*returnedLength = sizeof(MEMORY_BASIC_INFORMATION);
#endif
		return STATUS_INFO_LENGTH_MISMATCH;
	}

	Pin<kernel32::ProcessObject> process;
	if (!kernel32::isPseudoCurrentProcessHandle(processHandle)) {
		HandleMeta metadata{};
		auto object = wibo::handles().get(processHandle, &metadata);
		if (!object)
			return STATUS_INVALID_HANDLE;
		if (object->type != ObjectType::Process)
			return STATUS_OBJECT_TYPE_MISMATCH;
		process = std::move(object).downcast<kernel32::ProcessObject>();
		if (!(metadata.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)))
			return STATUS_ACCESS_DENIED;
		if (process->pid != getpid())
			return STATUS_NOT_SUPPORTED;
	}
	if (!information)
		return STATUS_ACCESS_VIOLATION;

	MEMORY_BASIC_INFORMATION result{};
	const DWORD incomingError = kernel32::getLastError();
	const SIZE_T written = kernel32::VirtualQuery(baseAddress, &result, sizeof(result));
	const DWORD queryError = kernel32::getLastError();
	kernel32::setLastError(incomingError);
	if (!written)
		return wibo::statusFromWinError(queryError);
	copyBasicInformation(information, result);
	if (returnedLength)
		*returnedLength = sizeof(result);
	return STATUS_SUCCESS;
}
