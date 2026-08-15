#include "toolhelp.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "modules.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace {

struct SnapshotModule {
	HMODULE handle = NO_HANDLE;
	GUEST_PTR baseAddress = GUEST_NULL;
	DWORD imageSize = 0;
	std::string name;
	std::string path;
};

struct ToolhelpSnapshotObject final : ObjectBase {
	static constexpr ObjectType kType = ObjectType::ToolhelpSnapshot;

	std::vector<SnapshotModule> modules;
	size_t nextModule = 0;

	ToolhelpSnapshotObject() : ObjectBase(kType) {}
};

template <size_t Capacity> void copyWideString(WCHAR (&destination)[Capacity], const std::string &source) {
	auto wide = stringToWideString(source.c_str());
	const size_t copyCount = std::min(wide.size(), Capacity - 1);
	std::copy_n(wide.data(), copyCount, destination);
	destination[copyCount] = 0;
}

bool writeModuleEntry(const SnapshotModule &module, LPMODULEENTRY32W output) {
	if (!output || output->dwSize < sizeof(MODULEENTRY32W)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	const DWORD requestedSize = output->dwSize;
	std::memset(output, 0, sizeof(*output));
	output->dwSize = requestedSize;
	output->th32ProcessID = static_cast<DWORD>(getpid());
	output->GlblcntUsage = 0xffff;
	output->ProccntUsage = 0xffff;
	output->modBaseAddr = module.baseAddress;
	output->modBaseSize = module.imageSize;
	output->hModule = module.handle;
	copyWideString(output->szModule, module.name);
	copyWideString(output->szExePath, module.path);
	return true;
}

} // namespace

namespace kernel32 {

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateToolhelp32Snapshot(0x%x, %u)\n", dwFlags, th32ProcessID);
	constexpr DWORD supportedFlags = TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32;
	if ((dwFlags & supportedFlags) == 0 || (dwFlags & ~supportedFlags) != 0 ||
		(th32ProcessID != 0 && th32ProcessID != static_cast<DWORD>(getpid()))) {
		setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	auto snapshot = make_pin<ToolhelpSnapshotObject>();
	std::unordered_set<wibo::ModuleInfo *> seen;
	for (const auto &[key, module] : wibo::allLoadedModules()) {
		(void)key;
		if (!module || !module->executable || !seen.insert(module.get()).second) {
			continue;
		}
		const size_t imageSize = module->executable->imageSize;
		SnapshotModule entry{};
		entry.handle = module->handle;
		entry.baseAddress = toGuestPtr(module->executable->imageBase);
		entry.imageSize = imageSize > std::numeric_limits<DWORD>::max() ? std::numeric_limits<DWORD>::max()
																		: static_cast<DWORD>(imageSize);
		entry.name = module->resolvedPath.empty() ? module->originalName : module->resolvedPath.filename().string();
		entry.path = module->resolvedPath.empty() ? module->originalName : module->resolvedPath.string();
		snapshot->modules.push_back(std::move(entry));
	}
	std::sort(
		snapshot->modules.begin(), snapshot->modules.end(),
		[](const SnapshotModule &left, const SnapshotModule &right) { return left.baseAddress < right.baseAddress; });

	HANDLE handle = wibo::handles().alloc(std::move(snapshot), 0, 0);
	DEBUG_LOG("-> %p\n", handle);
	return handle;
}

BOOL WINAPI Module32FirstW(HANDLE hSnapshot, LPMODULEENTRY32W lpme) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("Module32FirstW(%p, %p)\n", hSnapshot, lpme);
	auto snapshot = wibo::handles().getAs<ToolhelpSnapshotObject>(hSnapshot);
	if (!snapshot) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	snapshot->nextModule = 0;
	if (snapshot->modules.empty()) {
		setLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	if (!writeModuleEntry(snapshot->modules[0], lpme)) {
		return FALSE;
	}
	snapshot->nextModule = 1;
	return TRUE;
}

BOOL WINAPI Module32NextW(HANDLE hSnapshot, LPMODULEENTRY32W lpme) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("Module32NextW(%p, %p)\n", hSnapshot, lpme);
	auto snapshot = wibo::handles().getAs<ToolhelpSnapshotObject>(hSnapshot);
	if (!snapshot) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (snapshot->nextModule >= snapshot->modules.size()) {
		setLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	if (!writeModuleEntry(snapshot->modules[snapshot->nextModule], lpme)) {
		return FALSE;
	}
	++snapshot->nextModule;
	return TRUE;
}

} // namespace kernel32
