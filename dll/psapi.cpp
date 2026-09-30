#include "psapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "kernel32/libloaderapi.h"
#include "kernel32/processthreadsapi.h"
#include "modules.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#ifdef __APPLE__
#include <libproc.h>
#else
#include <fstream>
#endif

namespace {

enum class ProcessAccess { Modules, Memory };

bool validateCurrentProcess(HANDLE process, ProcessAccess access) {
	if (kernel32::isPseudoCurrentProcessHandle(process)) {
		return true;
	}
	HandleMeta metadata{};
	auto object = wibo::handles().getAs<kernel32::ProcessObject>(process, &metadata);
	if (!object || object->pid != getpid()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return false;
	}
	const DWORD granted = metadata.grantedAccess;
	bool allowed;
	if (access == ProcessAccess::Modules) {
		const DWORD required = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
		allowed = (granted & required) == required;
	} else {
		allowed = (granted & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)) != 0;
	}
	if (!allowed) {
		kernel32::setLastError(ERROR_ACCESS_DENIED);
	}
	return allowed;
}

std::string moduleBaseName(const wibo::ModuleInfo &module) {
	if (module.moduleStub)
		return module.normalizedName;
	if (module.resolvedPath.empty())
		return module.originalName;
	return module.resolvedPath.filename().string();
}

std::vector<wibo::ModulePtr> loadedModules() {
	std::vector<wibo::ModulePtr> result;
	std::unordered_set<wibo::ModuleInfo *> seen;
	for (const auto &[key, module] : wibo::allLoadedModules()) {
		(void)key;
		if (!module || (!module->executable && !module->moduleStub) || !seen.insert(module.get()).second) {
			continue;
		}
		result.push_back(module);
	}
	std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
		if (static_cast<bool>(left->executable) != static_cast<bool>(right->executable)) {
			return static_cast<bool>(left->executable);
		}
		if (left->executable) {
			return reinterpret_cast<uintptr_t>(left->executable->imageBase) <
				   reinterpret_cast<uintptr_t>(right->executable->imageBase);
		}
		return left->handle < right->handle;
	});
	return result;
}

bool processMemoryUsage(uint64_t &workingSet, uint64_t &peakWorkingSet, DWORD &faults) {
	struct rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return false;
#ifdef __APPLE__
	proc_taskinfo task{};
	if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task))
		return false;
	workingSet = task.pti_resident_size;
	peakWorkingSet = std::max<uint64_t>(workingSet, usage.ru_maxrss);
	faults = task.pti_faults < 0 ? 0 : static_cast<DWORD>(task.pti_faults);
#else
	std::ifstream statm("/proc/self/statm");
	uint64_t pages = 0, residentPages = 0;
	if (!(statm >> pages >> residentPages))
		return false;
	const long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0)
		return false;
	workingSet = residentPages * static_cast<uint64_t>(pageSize);
	peakWorkingSet = std::max<uint64_t>(workingSet, static_cast<uint64_t>(usage.ru_maxrss) * 1024);
	const uint64_t totalFaults = static_cast<uint64_t>(usage.ru_minflt) + static_cast<uint64_t>(usage.ru_majflt);
	faults = static_cast<DWORD>(std::min<uint64_t>(totalFaults, std::numeric_limits<DWORD>::max()));
#endif
	return true;
}

SIZE_T guestSize(uint64_t value) {
	return static_cast<SIZE_T>(std::min<uint64_t>(value, std::numeric_limits<SIZE_T>::max()));
}

} // namespace

namespace psapi {

BOOL WINAPI EnumProcessModules(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EnumProcessModules(%p, %p, %u, %p)\n", hProcess, lphModule, cb, lpcbNeeded);
	if (!validateCurrentProcess(hProcess, ProcessAccess::Modules) || !lpcbNeeded || (cb != 0 && !lphModule)) {
		if (lpcbNeeded == nullptr || (cb != 0 && lphModule == nullptr)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
		}
		return FALSE;
	}

	auto modules = loadedModules();
	const size_t required = modules.size() * sizeof(HMODULE);
	*lpcbNeeded =
		required > std::numeric_limits<DWORD>::max() ? std::numeric_limits<DWORD>::max() : static_cast<DWORD>(required);
	const size_t capacity = cb / sizeof(HMODULE);
	const size_t count = std::min(capacity, modules.size());
	for (size_t i = 0; i < count; ++i) {
		lphModule[i] = modules[i]->handle;
	}
	return TRUE;
}

DWORD WINAPI GetModuleBaseNameA(HANDLE hProcess, HMODULE hModule, LPSTR lpBaseName, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleBaseNameA(%p, %p, %p, %u)\n", hProcess, hModule, lpBaseName, nSize);
	if (!validateCurrentProcess(hProcess, ProcessAccess::Modules) || !lpBaseName || nSize == 0) {
		if (!lpBaseName || nSize == 0) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
		}
		return 0;
	}

	wibo::ModuleInfo *module = wibo::moduleInfoFromHandle(hModule);
	if (!module) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	const std::string name = moduleBaseName(*module);
	const size_t count = std::min(name.size(), static_cast<size_t>(nSize - 1));
	std::memcpy(lpBaseName, name.data(), count);
	lpBaseName[count] = '\0';
	return static_cast<DWORD>(count);
}

DWORD WINAPI GetModuleBaseNameW(HANDLE hProcess, HMODULE hModule, LPWSTR lpBaseName, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleBaseNameW(%p, %p, %p, %u)\n", hProcess, hModule, lpBaseName, nSize);
	if (!validateCurrentProcess(hProcess, ProcessAccess::Modules) || !lpBaseName || nSize == 0) {
		if (!lpBaseName || nSize == 0) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
		}
		return 0;
	}

	wibo::ModuleInfo *module = wibo::moduleInfoFromHandle(hModule);
	if (!module) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	const std::string name = moduleBaseName(*module);
	const std::u16string wideName = utf8ToUtf16(name).value_or(stringToUtf16(name));
	const size_t count = std::min(wideName.size(), static_cast<size_t>(nSize - 1));
	for (size_t i = 0; i < count; ++i) {
		lpBaseName[i] = wideName[i];
	}
	lpBaseName[count] = 0;
	return static_cast<DWORD>(count);
}

DWORD WINAPI GetModuleFileNameExA(HANDLE hProcess, HMODULE hModule, LPSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameExA(%p, %p, %p, %u)\n", hProcess, hModule, lpFilename, nSize);
	if (!validateCurrentProcess(hProcess, hModule ? ProcessAccess::Modules : ProcessAccess::Memory))
		return 0;
	return kernel32::GetModuleFileNameA(hModule, lpFilename, nSize);
}

DWORD WINAPI GetModuleFileNameExW(HANDLE hProcess, HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameExW(%p, %p, %p, %u)\n", hProcess, hModule, lpFilename, nSize);
	if (!validateCurrentProcess(hProcess, hModule ? ProcessAccess::Modules : ProcessAccess::Memory))
		return 0;
	return kernel32::GetModuleFileNameW(hModule, lpFilename, nSize);
}

BOOL WINAPI GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO lpmodinfo, DWORD cb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleInformation(%p, %p, %p, %u)\n", hProcess, hModule, lpmodinfo, cb);
	if (!validateCurrentProcess(hProcess, ProcessAccess::Modules) || !lpmodinfo || cb < sizeof(MODULEINFO)) {
		if (!lpmodinfo || cb < sizeof(MODULEINFO)) {
			kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		}
		return FALSE;
	}

	wibo::ModuleInfo *module = wibo::moduleInfoFromHandle(hModule);
	if (!module || !module->executable) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	lpmodinfo->lpBaseOfDll = toGuestPtr(module->executable->imageBase);
	lpmodinfo->SizeOfImage = module->executable->imageSize > std::numeric_limits<DWORD>::max()
								 ? std::numeric_limits<DWORD>::max()
								 : static_cast<DWORD>(module->executable->imageSize);
	lpmodinfo->EntryPoint = toGuestPtr(module->executable->entryPoint);
	return TRUE;
}

BOOL WINAPI GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD cb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessMemoryInfo(%p, %p, %u)\n", process, counters, cb);
	if (!validateCurrentProcess(process, ProcessAccess::Memory))
		return FALSE;
	if (!counters || cb < sizeof(PROCESS_MEMORY_COUNTERS)) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) {
		// The available host counters do not measure private commit charge.
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	uint64_t workingSet = 0, peakWorkingSet = 0;
	DWORD faults = 0;
	if (!processMemoryUsage(workingSet, peakWorkingSet, faults)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	PROCESS_MEMORY_COUNTERS result{};
	result.cb = sizeof(result);
	result.PageFaultCount = faults;
	result.PeakWorkingSetSize = guestSize(peakWorkingSet);
	result.WorkingSetSize = guestSize(workingSet);
	std::memcpy(counters, &result, sizeof(result));
	return TRUE;
}

} // namespace psapi

#include "psapi_trampolines.h"

extern const wibo::ModuleStub lib_psapi = {
	(const char *[]){
		"psapi",
		nullptr,
	},
	psapiThunkByName,
	nullptr,
};
