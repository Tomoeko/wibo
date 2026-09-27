#include "winbase.h"
#include "winnls.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "internal.h"
#include "mimalloc/types.h"
#include "modules.h"
#include "strutil.h"
#include "types.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mimalloc.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/statvfs.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT GMEM_MOVEABLE = 0x0002;
constexpr UINT GMEM_ZEROINIT = 0x0040;
constexpr UINT GMEM_MODIFY = 0x0080;

constexpr UINT LMEM_MOVEABLE = 0x0002;
constexpr UINT LMEM_ZEROINIT = 0x0040;

constexpr ATOM kMinIntegerAtom = 0x0001;
constexpr ATOM kMaxIntegerAtom = 0xBFFF;
constexpr ATOM kMinStringAtom = 0xC000;
constexpr ATOM kMaxStringAtom = 0xFFFF;

bool memoryProtectionAllowsRead(DWORD protect) {
	if ((protect & PAGE_GUARD) != 0) {
		return false;
	}
	switch (protect & 0xff) {
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool memoryProtectionAllowsWrite(DWORD protect) {
	if ((protect & PAGE_GUARD) != 0) {
		return false;
	}
	switch (protect & 0xff) {
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

bool queryAccessibleSpan(const void *address, bool write, uintptr_t &spanEnd) {
	MEMORY_BASIC_INFORMATION info{};
	if (wibo::heap::virtualQuery(address, &info) != wibo::heap::VmStatus::Success || info.State != MEM_COMMIT ||
		(write ? !memoryProtectionAllowsWrite(info.Protect) : !memoryProtectionAllowsRead(info.Protect))) {
		return false;
	}

	const uintptr_t start = reinterpret_cast<uintptr_t>(address);
	const uintptr_t regionBase = static_cast<uintptr_t>(info.BaseAddress);
	if (info.RegionSize > std::numeric_limits<uintptr_t>::max() - regionBase) {
		spanEnd = std::numeric_limits<uintptr_t>::max();
	} else {
		spanEnd = regionBase + static_cast<uintptr_t>(info.RegionSize);
	}
	return start >= regionBase && start < spanEnd;
}

bool isBadMemoryRange(const void *address, UINT_PTR size, bool write) {
	if (size == 0) {
		return false;
	}
	if (!address) {
		return true;
	}

	uintptr_t current = reinterpret_cast<uintptr_t>(address);
	if (size > std::numeric_limits<uintptr_t>::max() - current) {
		return true;
	}
	const uintptr_t requestedEnd = current + static_cast<uintptr_t>(size);
	while (current < requestedEnd) {
		uintptr_t spanEnd = 0;
		if (!queryAccessibleSpan(reinterpret_cast<const void *>(current), write, spanEnd)) {
			return true;
		}
		current = std::min(spanEnd, requestedEnd);
	}
	return false;
}

template <typename Char> bool isBadString(const Char *string, UINT_PTR maxCharacters) {
	if (maxCharacters == 0) {
		return false;
	}
	if (!string || maxCharacters > std::numeric_limits<uintptr_t>::max() / sizeof(Char)) {
		return true;
	}

	uintptr_t current = reinterpret_cast<uintptr_t>(string);
	UINT_PTR remaining = maxCharacters;
	while (remaining != 0) {
		uintptr_t spanEnd = 0;
		if (!queryAccessibleSpan(reinterpret_cast<const void *>(current), false, spanEnd)) {
			return true;
		}
		const uintptr_t availableBytes = spanEnd - current;
		const UINT_PTR availableCharacters = static_cast<UINT_PTR>(availableBytes / sizeof(Char));
		const UINT_PTR inspectCount = std::min(remaining, availableCharacters);
		if (inspectCount == 0) {
			return true;
		}

		const auto *characters = reinterpret_cast<const Char *>(current);
		for (UINT_PTR index = 0; index < inspectCount; ++index) {
			if (characters[index] == 0) {
				return false;
			}
		}
		remaining -= inspectCount;
		if (inspectCount > (std::numeric_limits<uintptr_t>::max() - current) / sizeof(Char)) {
			return true;
		}
		current += inspectCount * sizeof(Char);
	}
	return false;
}

SIZE_T clampToSizeT(uint64_t value) {
	constexpr uint64_t kMaxSizeT = static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max());
	return value > kMaxSizeT ? static_cast<SIZE_T>(kMaxSizeT) : static_cast<SIZE_T>(value);
}

struct MemorySnapshot {
	uint64_t totalPhys = 0;
	uint64_t availPhys = 0;
	uint64_t totalPageFile = 0;
	uint64_t availPageFile = 0;
};

bool queryHostMemory(MemorySnapshot &out) {
#if defined(__linux__)
	struct sysinfo info{};
	if (sysinfo(&info) != 0) {
		return false;
	}
	const uint64_t unit = info.mem_unit ? static_cast<uint64_t>(info.mem_unit) : 1;
	out.totalPhys = static_cast<uint64_t>(info.totalram) * unit;
	out.availPhys = static_cast<uint64_t>(info.freeram) * unit;
	out.totalPageFile = (static_cast<uint64_t>(info.totalram) + static_cast<uint64_t>(info.totalswap)) * unit;
	out.availPageFile = (static_cast<uint64_t>(info.freeram) + static_cast<uint64_t>(info.freeswap)) * unit;
	if (info.bufferram > 0) {
		uint64_t buffers = static_cast<uint64_t>(info.bufferram) * unit;
		out.availPhys += buffers;
		out.availPageFile += buffers;
	}
	return true;
#elif defined(__APPLE__)
	uint64_t totalPhys = 0;
	size_t totalPhysSize = sizeof(totalPhys);
	if (sysctlbyname("hw.memsize", &totalPhys, &totalPhysSize, nullptr, 0) != 0) {
		return false;
	}
	vm_size_t pageSize = 0;
	if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS || pageSize == 0) {
		return false;
	}
	vm_statistics64_data_t vmstat{};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vmstat), &count) !=
		KERN_SUCCESS) {
		return false;
	}
	uint64_t freePages = static_cast<uint64_t>(vmstat.free_count) + static_cast<uint64_t>(vmstat.inactive_count) +
						 static_cast<uint64_t>(vmstat.speculative_count);
	out.totalPhys = totalPhys;
	out.availPhys = freePages * static_cast<uint64_t>(pageSize);

	struct xsw_usage swap{};
	size_t swapSize = sizeof(swap);
	if (sysctlbyname("vm.swapusage", &swap, &swapSize, nullptr, 0) == 0 && swapSize == sizeof(swap)) {
		out.totalPageFile = swap.xsu_total;
		out.availPageFile = swap.xsu_avail;
	} else {
		out.totalPageFile = totalPhys;
		out.availPageFile = out.availPhys;
	}
	return true;
#else
	return false;
#endif
}

struct AtomData {
	uint16_t refCount = 0;
	std::string original;
};

struct AtomTable {
	std::mutex mutex;
	std::unordered_map<std::string, ATOM> stringToAtom;
	std::unordered_map<ATOM, AtomData> atomToData;
	ATOM nextStringAtom = kMinStringAtom;
};

AtomTable &localAtomTable() {
	static AtomTable table;
	return table;
}

template <typename Table> ATOM allocateStringAtomLocked(Table &table) {
	constexpr unsigned int kRange = static_cast<unsigned int>(kMaxStringAtom - kMinStringAtom + 1);
	unsigned int startOffset = 0;
	if (table.nextStringAtom >= kMinStringAtom && table.nextStringAtom <= kMaxStringAtom) {
		startOffset = static_cast<unsigned int>(table.nextStringAtom - kMinStringAtom);
	}
	for (unsigned int i = 0; i < kRange; ++i) {
		unsigned int offset = (startOffset + i) % kRange;
		ATOM candidate = static_cast<ATOM>(kMinStringAtom + offset);
		if (table.atomToData.find(candidate) == table.atomToData.end()) {
			table.nextStringAtom = static_cast<ATOM>(candidate + 1);
			if (table.nextStringAtom > kMaxStringAtom) {
				table.nextStringAtom = kMinStringAtom;
			}
			return candidate;
		}
	}
	return 0;
}

bool tryHandleIntegerAtomPointer(const void *ptr, ATOM &atomOut) {
	uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
	if ((value >> 16) != 0) {
		return false;
	}
	ATOM maybeAtom = static_cast<ATOM>(value & 0xFFFFu);
	if (maybeAtom < kMinIntegerAtom || maybeAtom > kMaxIntegerAtom) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		atomOut = 0;
		return true;
	}
	atomOut = maybeAtom;
	return true;
}

ATOM findAtomByNormalizedKey(const std::string &normalizedKey) {
	auto &table = localAtomTable();
	std::lock_guard lk(table.mutex);
	auto it = table.stringToAtom.find(normalizedKey);
	if (it == table.stringToAtom.end()) {
		kernel32::setLastError(ERROR_FILE_NOT_FOUND);
		return 0;
	}
	return it->second;
}

ATOM tryParseIntegerAtomString(const std::string &value, bool &handled) {
	handled = false;
	if (value.empty() || value[0] != '#') {
		return 0;
	}
	char *end = nullptr;
	unsigned long parsed = std::strtoul(value.c_str() + 1, &end, 10);
	if (end == value.c_str() + 1 || *end != '\0') {
		return 0;
	}
	handled = true;
	if (parsed < kMinIntegerAtom || parsed > kMaxIntegerAtom) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return static_cast<ATOM>(parsed);
}

ATOM findAtomByString(const std::string &value) {
	bool handledInteger = false;
	ATOM atom = tryParseIntegerAtomString(value, handledInteger);
	if (handledInteger) {
		return atom;
	}
	std::string normalized = stringToLower(value);
	return findAtomByNormalizedKey(normalized);
}

ATOM addAtomByString(const std::string &value) {
	bool handledInteger = false;
	ATOM atom = tryParseIntegerAtomString(value, handledInteger);
	if (handledInteger) {
		return atom;
	}
	if (value.empty() || value.size() > 255) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string normalized = stringToLower(value);
	auto &table = localAtomTable();
	std::lock_guard lk(table.mutex);
	auto existing = table.stringToAtom.find(normalized);
	if (existing != table.stringToAtom.end()) {
		auto dataIt = table.atomToData.find(existing->second);
		if (dataIt != table.atomToData.end() && dataIt->second.refCount < std::numeric_limits<uint16_t>::max()) {
			dataIt->second.refCount++;
		}
		return existing->second;
	}
	ATOM newAtom = allocateStringAtomLocked(table);
	if (newAtom == 0) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	AtomData data;
	data.refCount = 1;
	data.original = value;
	table.stringToAtom.emplace(std::move(normalized), newAtom);
	table.atomToData.emplace(newAtom, std::move(data));
	return newAtom;
}

// This is a separate namespace from local atoms and is shared by threads in
// this wibo process only. Windows shares global atoms across processes and
// retains them after process exit; there is no host-wide atom service here.
struct GlobalAtomData {
	uint32_t refCount = 1;
	std::u16string original;
};

struct GlobalAtomTable {
	std::mutex mutex;
	std::unordered_map<std::u16string, ATOM> stringToAtom;
	std::unordered_map<ATOM, GlobalAtomData> atomToData;
	ATOM nextStringAtom = kMinStringAtom;
};

GlobalAtomTable &globalAtomTable() {
	static GlobalAtomTable table;
	return table;
}

std::u16string normalizeGlobalAtomName(std::u16string value) {
	// Deterministic Basic Latin / Latin-1 uppercasing for the current ACP.
	// Preserve y-diaeresis's UTF-16 uppercase form outside Latin-1. Wine's atom
	// table keeps micro sign distinct from Greek Mu. Other Unicode case mappings
	// remain unsupported.
	for (auto &character : value) {
		if ((character >= u'a' && character <= u'z') || (character >= 0xe0 && character <= 0xf6) ||
			(character >= 0xf8 && character <= 0xfe)) {
			character -= 0x20;
		} else if (character == 0xff) {
			character = 0x0178;
		}
	}
	return value;
}

ATOM globalAtomByString(const std::u16string &value, bool add) {
	if (value.empty() || value.size() > 255) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	// Only a '#' followed entirely by decimal digits denotes an integer.
	// strtoul would incorrectly accept signs and leading whitespace.
	if (value.size() > 1 && value[0] == u'#' &&
		std::all_of(value.begin() + 1, value.end(), [](char16_t ch) { return ch >= u'0' && ch <= u'9'; })) {
		unsigned int number = 0;
		for (size_t index = 1; index < value.size(); ++index) {
			number = number * 10 + static_cast<unsigned int>(value[index] - u'0');
			if (number > kMaxIntegerAtom) {
				kernel32::setLastError(ERROR_INVALID_PARAMETER);
				return 0;
			}
		}
		if (number == 0) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
		}
		return static_cast<ATOM>(number);
	}

	auto normalized = normalizeGlobalAtomName(value);
	auto &table = globalAtomTable();
	std::lock_guard lk(table.mutex);
	auto existing = table.stringToAtom.find(normalized);
	if (existing != table.stringToAtom.end()) {
		if (add) {
			auto &data = table.atomToData.at(existing->second);
			if (data.refCount == std::numeric_limits<uint32_t>::max()) {
				kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
				return 0;
			}
			++data.refCount;
		}
		return existing->second;
	}
	if (!add) {
		kernel32::setLastError(ERROR_FILE_NOT_FOUND);
		return 0;
	}
	ATOM atom = allocateStringAtomLocked(table);
	if (!atom) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	table.stringToAtom.emplace(std::move(normalized), atom);
	table.atomToData.emplace(atom, GlobalAtomData{1, value});
	return atom;
}

ATOM globalAtomA(LPCSTR string, bool add) {
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(string, atom)) {
		return atom;
	}
	size_t length = strnlen(string, 256);
	if (length == 0 || length > 255) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	// GetACP currently advertises ISO-8859-1. Keep that one-byte mapping
	// explicit; this does not implement other Windows ANSI code pages.
	std::u16string value;
	value.reserve(length);
	for (size_t index = 0; index < length; ++index) {
		value.push_back(static_cast<unsigned char>(string[index]));
	}
	return globalAtomByString(value, add);
}

ATOM globalAtomW(LPCWSTR string, bool add) {
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(string, atom)) {
		return atom;
	}
	size_t length = wstrnlen(string, 256);
	if (length == 0 || length > 255) {
		kernel32::setLastError(length == 0 ? ERROR_INVALID_NAME : ERROR_INVALID_PARAMETER);
		return 0;
	}
	return globalAtomByString(std::u16string(string, string + length), add);
}

bool getGlobalAtomName(ATOM atom, std::u16string &value) {
	if (atom >= kMinIntegerAtom && atom <= kMaxIntegerAtom) {
		auto number = std::to_string(atom);
		value = u'#';
		value.append(number.begin(), number.end());
		return true;
	}
	auto &table = globalAtomTable();
	std::lock_guard lk(table.mutex);
	auto found = table.atomToData.find(atom);
	if (found == table.atomToData.end()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return false;
	}
	value = found->second.original;
	return true;
}

bool tryGetCurrentDirectoryPath(std::string &outPath) {
	std::error_code ec;
	std::filesystem::path cwd = std::filesystem::current_path(ec);
	if (ec) {
		kernel32::setLastError(wibo::winErrorFromErrno(ec.value()));
		return false;
	}
	outPath = files::pathToWindows(cwd);
	return true;
}

bool computeLongWindowsPath(const std::string &inputPath, std::string &longPath) {
	bool hasTrailingSlash = false;
	if (!inputPath.empty()) {
		char last = inputPath.back();
		hasTrailingSlash = (last == '\\' || last == '/');
	}

	auto hostPath = files::pathFromWindows(inputPath.c_str());
	if (hostPath.empty()) {
		kernel32::setLastError(ERROR_PATH_NOT_FOUND);
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::exists(hostPath, ec)) {
		kernel32::setLastError(ERROR_FILE_NOT_FOUND);
		return false;
	}

	longPath = files::pathToWindows(hostPath);
	if (hasTrailingSlash && !longPath.empty() && longPath.back() != '\\') {
		longPath.push_back('\\');
	}
	return true;
}

bool resolveDiskFreeSpaceStat(const char *rootPathName, struct statvfs &outBuf, std::string &resolvedPath) {
	std::filesystem::path hostPath;
	if (rootPathName && *rootPathName) {
		hostPath = files::pathFromWindows(rootPathName);
	} else {
		std::error_code ec;
		hostPath = std::filesystem::current_path(ec);
		if (ec) {
			kernel32::setLastError(ERROR_PATH_NOT_FOUND);
			return false;
		}
	}
	if (hostPath.empty()) {
		kernel32::setLastError(ERROR_PATH_NOT_FOUND);
		return false;
	}

	hostPath = hostPath.lexically_normal();
	if (hostPath.empty()) {
		hostPath = std::filesystem::path("/");
	}

	std::error_code ec;
	if (!hostPath.is_absolute()) {
		auto abs = std::filesystem::absolute(hostPath, ec);
		if (ec) {
			kernel32::setLastError(ERROR_PATH_NOT_FOUND);
			return false;
		}
		hostPath = abs;
	}

	std::filesystem::path queryPath = hostPath;
	while (true) {
		std::string query = queryPath.empty() ? std::string("/") : queryPath.string();
		if (query.empty()) {
			query = "/";
		}
		if (statvfs(query.c_str(), &outBuf) == 0) {
			resolvedPath = query;
			return true;
		}

		int savedErrno = errno;
		if (savedErrno != ENOENT && savedErrno != ENOTDIR) {
			kernel32::setLastError(wibo::winErrorFromErrno(savedErrno));
			return false;
		}

		std::filesystem::path parent = queryPath.parent_path();
		if (parent == queryPath) {
			kernel32::setLastError(wibo::winErrorFromErrno(savedErrno));
			return false;
		}
		if (parent.empty()) {
			parent = std::filesystem::path("/");
		}
		queryPath = parent;
	}
}

constexpr DWORD kComputerNameLength = 8;
constexpr DWORD kComputerNameRequiredSize = kComputerNameLength + 1;
constexpr const char kComputerNameAnsi[] = "COMPNAME";
const uint16_t kComputerNameWide[] = {u'C', u'O', u'M', u'P', u'N', u'A', u'M', u'E', 0};

struct DllRedirectionEntry {
	std::string nameLower;
	wibo::heap::guest_ptr<ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION> dllData;
};

struct ActivationContext {
	std::vector<DllRedirectionEntry> dllRedirections;
};

wibo::heap::guest_ptr<ActivationContext> g_builtinActCtx;

ActivationContext *currentActivationContext() {
	if (!g_builtinActCtx) {
		g_builtinActCtx = wibo::heap::make_guest_unique<ActivationContext>();
	}
	return g_builtinActCtx.get();
}

} // namespace

void ensureDefaultActivationContext() {
	static std::once_flag initFlag;
	std::call_once(initFlag, [] {
		ActivationContext *ctx = currentActivationContext();
		auto addDll = [ctx](const std::string &name) {
			std::string lowerName = stringToLower(name);
			for (const auto &entry : ctx->dllRedirections) {
				if (entry.nameLower == lowerName) {
					return;
				}
			}

			DllRedirectionEntry entry;
			entry.nameLower = std::move(lowerName);
			entry.dllData = wibo::heap::make_guest_unique<ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION>();
			entry.dllData->Size = sizeof(ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION);
			entry.dllData->Flags = ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION_PATH_OMITS_ASSEMBLY_ROOT;
			entry.dllData->TotalPathLength = 0;
			entry.dllData->PathSegmentCount = 0;
			entry.dllData->PathSegmentOffset = 0;
			ctx->dllRedirections.emplace_back(std::move(entry));
		};
		addDll("msvcr80.dll");
		addDll("msvcp80.dll");
		for (const auto &[key, module] : wibo::allLoadedModules()) {
			if (!module->moduleStub) {
				addDll(module->normalizedName);
			}
		}
	});
}

namespace kernel32 {

namespace {

// Match the existing fixed-locale CompareStringA facade without routing UTF-16
// through CompareStringW's current byte conversion. Every code unit participates
// in equality/order; insensitive comparisons fold ASCII only. This is not the
// complete Windows locale-sensitive word sort (case weights, punctuation and
// non-ASCII folding still need an NLS implementation).
int compareLegacyWideStrings(LPCWSTR first, LPCWSTR second, bool ignoreCase) {
	if (!first || !second) {
		setLastError(ERROR_INVALID_PARAMETER);
		return -2;
	}
	for (size_t index = 0;; ++index) {
		uint16_t left = first[index];
		uint16_t right = second[index];
		if (ignoreCase) {
			if (left >= 'a' && left <= 'z') {
				left -= 'a' - 'A';
			}
			if (right >= 'a' && right <= 'z') {
				right -= 'a' - 'A';
			}
		}
		if (left != right) {
			return left < right ? -1 : 1;
		}
		if (left == 0) {
			return 0;
		}
	}
}

} // namespace

int WINAPI lstrlenA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrlenA(%p)\n", lpString);
	if (!lpString) {
		// The documented return is zero; Wine also reports this error.
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return static_cast<int>(std::strlen(lpString));
}

int WINAPI lstrlenW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrlenW(%p)\n", lpString);
	if (!lpString) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return static_cast<int>(wstrlen(lpString));
}

int WINAPI lstrcmpA(LPCSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpA(%p, %p)\n", lpString1, lpString2);
	// Use the same fixed user locale/ACP as wibo's existing NLS facade.
	return CompareStringA(GetUserDefaultLCID(), 0, lpString1, -1, lpString2, -1) - 2;
}

int WINAPI lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpW(%p, %p)\n", lpString1, lpString2);
	return compareLegacyWideStrings(lpString1, lpString2, false);
}

int WINAPI lstrcmpiA(LPCSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpiA(%p, %p)\n", lpString1, lpString2);
	constexpr DWORD kNormIgnoreCase = 0x00000001;
	return CompareStringA(GetUserDefaultLCID(), kNormIgnoreCase, lpString1, -1, lpString2, -1) - 2;
}

int WINAPI lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpiW(%p, %p)\n", lpString1, lpString2);
	return compareLegacyWideStrings(lpString1, lpString2, true);
}

LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcpynA(%p, %p, %d)\n", lpString1, lpString2, iMaxLength);

	UINT count = static_cast<UINT>(iMaxLength);
	if (!lpString1) {
		return nullptr;
	}
	if (!lpString2 && count > 1) {
		return nullptr;
	}

	LPSTR destination = lpString1;
	LPCSTR source = lpString2;
	while (count > 1 && *source) {
		count--;
		*destination++ = *source++;
	}
	if (count) {
		*destination = '\0';
	}
	return lpString1;
}

ATOM WINAPI AddAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, atom)) {
		DEBUG_LOG("AddAtomA(int:%u)\n", atom);
		return atom;
	}
	DEBUG_LOG("AddAtomA(%s)\n", lpString ? lpString : "<null>");
	if (!lpString) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = strnlen(lpString, 256);
	if (len == 0 || len >= 256) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string value(lpString, len);
	ATOM result = addAtomByString(value);
	DEBUG_LOG("AddAtomA -> %u (lastError=%u)\n", result, getLastError());
	return result;
}

ATOM WINAPI AddAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, atom)) {
		DEBUG_LOG("AddAtomW(int:%u)\n", atom);
		return atom;
	}
	if (!lpString) {
		DEBUG_LOG("AddAtomW(<null>)\n");
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = wstrnlen(reinterpret_cast<const uint16_t *>(lpString), 256);
	if (len == 0 || len >= 256) {
		DEBUG_LOG("AddAtomW(invalid length)\n");
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string value = wideStringToString(reinterpret_cast<const uint16_t *>(lpString), static_cast<int>(len));
	DEBUG_LOG("AddAtomW(%s)\n", value.c_str());
	ATOM result = addAtomByString(value);
	DEBUG_LOG("AddAtomW -> %u (lastError=%u)\n", result, getLastError());
	return result;
}

ATOM WINAPI FindAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, atom)) {
		DEBUG_LOG("FindAtomA(int:%u)\n", atom);
		return atom;
	}
	DEBUG_LOG("FindAtomA(%s)\n", lpString ? lpString : "<null>");
	if (!lpString) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = strnlen(lpString, 256);
	if (len == 0 || len >= 256) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string value(lpString, len);
	ATOM result = findAtomByString(value);
	DEBUG_LOG("FindAtomA -> %u (lastError=%u)\n", result, getLastError());
	return result;
}

ATOM WINAPI FindAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, atom)) {
		DEBUG_LOG("FindAtomW(int:%u)\n", atom);
		return atom;
	}
	if (!lpString) {
		DEBUG_LOG("FindAtomW(<null>)\n");
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = wstrnlen(reinterpret_cast<const uint16_t *>(lpString), 256);
	if (len == 0 || len >= 256) {
		DEBUG_LOG("FindAtomW(invalid length)\n");
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string value = wideStringToString(reinterpret_cast<const uint16_t *>(lpString), static_cast<int>(len));
	DEBUG_LOG("FindAtomW(%s)\n", value.c_str());
	ATOM result = findAtomByString(value);
	DEBUG_LOG("FindAtomW -> %u (lastError=%u)\n", result, getLastError());
	return result;
}

UINT WINAPI GetAtomNameA(ATOM nAtom, LPSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAtomNameA(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (!lpBuffer || nSize <= 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string value;
	if (nAtom >= kMinIntegerAtom && nAtom <= kMaxIntegerAtom) {
		value = '#';
		value += std::to_string(nAtom);
	} else {
		auto &table = localAtomTable();
		std::lock_guard lk(table.mutex);
		auto it = table.atomToData.find(nAtom);
		if (it == table.atomToData.end()) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		value = it->second.original;
	}
	if (value.size() + 1 > static_cast<size_t>(nSize)) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	std::memcpy(lpBuffer, value.c_str(), value.size());
	lpBuffer[value.size()] = '\0';
	UINT written = static_cast<UINT>(value.size());
	DEBUG_LOG("GetAtomNameA -> %u (lastError=%u)\n", written, getLastError());
	return written;
}

UINT WINAPI GetAtomNameW(ATOM nAtom, LPWSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAtomNameW(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (!lpBuffer || nSize <= 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string narrow;
	if (nAtom >= kMinIntegerAtom && nAtom <= kMaxIntegerAtom) {
		narrow = '#';
		narrow += std::to_string(nAtom);
	} else {
		auto &table = localAtomTable();
		std::lock_guard lk(table.mutex);
		auto it = table.atomToData.find(nAtom);
		if (it == table.atomToData.end()) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		narrow = it->second.original;
	}
	auto wide = stringToWideString(narrow.c_str(), narrow.size());
	size_t needed = wide.size();
	if (needed > static_cast<size_t>(nSize)) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	std::memcpy(lpBuffer, wide.data(), needed * sizeof(uint16_t));
	if (needed > 0) {
		lpBuffer[needed - 1] = 0;
	}
	UINT written = static_cast<UINT>(needed ? needed - 1 : 0);
	DEBUG_LOG("GetAtomNameW -> %u (lastError=%u)\n", written, getLastError());
	return written;
}

ATOM WINAPI GlobalAddAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = globalAtomA(lpString, true);
	DEBUG_LOG("GlobalAddAtomA(%p) -> %u (lastError=%u)\n", lpString, atom, getLastError());
	return atom;
}

ATOM WINAPI GlobalAddAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = globalAtomW(lpString, true);
	DEBUG_LOG("GlobalAddAtomW(%p) -> %u (lastError=%u)\n", lpString, atom, getLastError());
	return atom;
}

ATOM WINAPI GlobalFindAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = globalAtomA(lpString, false);
	DEBUG_LOG("GlobalFindAtomA(%p) -> %u (lastError=%u)\n", lpString, atom, getLastError());
	return atom;
}

ATOM WINAPI GlobalFindAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = globalAtomW(lpString, false);
	DEBUG_LOG("GlobalFindAtomW(%p) -> %u (lastError=%u)\n", lpString, atom, getLastError());
	return atom;
}

ATOM WINAPI GlobalDeleteAtom(ATOM nAtom) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalDeleteAtom(%u)\n", nAtom);
	if (nAtom < kMinStringAtom) {
		return 0;
	}
	auto &table = globalAtomTable();
	std::lock_guard lk(table.mutex);
	auto found = table.atomToData.find(nAtom);
	if (found == table.atomToData.end()) {
		setLastError(ERROR_INVALID_HANDLE);
		// The documented return is always zero. Wine returns nAtom here;
		// callers must inspect last error to distinguish this failure.
		return 0;
	}
	if (--found->second.refCount == 0) {
		table.stringToAtom.erase(normalizeGlobalAtomName(found->second.original));
		table.atomToData.erase(found);
	}
	return 0;
}

UINT WINAPI GlobalGetAtomNameA(ATOM nAtom, LPSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalGetAtomNameA(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (nSize < 0 || (!lpBuffer && nSize > 0)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::u16string value;
	if (!getGlobalAtomName(nAtom, value)) {
		return 0;
	}
	size_t copied = nSize > 0 ? std::min(value.size(), static_cast<size_t>(nSize - 1)) : 0;
	for (size_t index = 0; index < copied; ++index) {
		// Characters outside the current single-byte ACP use its default char.
		lpBuffer[index] = value[index] <= 0xff ? static_cast<char>(value[index]) : '?';
	}
	if (nSize > 0) {
		lpBuffer[copied] = 0;
	}
	if (value.size() >= static_cast<size_t>(nSize)) {
		setLastError(ERROR_MORE_DATA);
		return 0;
	}
	return static_cast<UINT>(copied);
}

UINT WINAPI GlobalGetAtomNameW(ATOM nAtom, LPWSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalGetAtomNameW(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (nSize < 0 || (!lpBuffer && nSize > 0)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::u16string value;
	if (!getGlobalAtomName(nAtom, value)) {
		return 0;
	}
	size_t copied = std::min(value.size(), static_cast<size_t>(nSize));
	if (copied > 0) {
		std::copy_n(value.begin(), copied, lpBuffer);
	}
	if (value.size() >= static_cast<size_t>(nSize)) {
		// Wine's wide API returns the unterminated prefix length on truncation,
		// unlike its ANSI API, which returns zero and terminates that prefix.
		setLastError(ERROR_MORE_DATA);
	} else {
		lpBuffer[copied] = 0;
	}
	return static_cast<UINT>(copied);
}

UINT WINAPI SetHandleCount(UINT uNumber) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetHandleCount(%u)\n", uNumber);
	(void)uNumber;
	return 0x3FFE;
}

PVOID WINAPI EncodePointer(PVOID Ptr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EncodePointer(%p)\n", Ptr);
	return Ptr;
}

PVOID WINAPI DecodePointer(PVOID Ptr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DecodePointer(%p)\n", Ptr);
	return Ptr;
}

DWORD WINAPI GetDllDirectoryA(DWORD nBufferLength, LPSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDllDirectoryA(%u, %p)\n", nBufferLength, lpBuffer);
	const auto specifiedName = wibo::dllDirectoryName();
	std::string name;
	name.reserve(specifiedName.size());
	// The current ANSI code page is ISO-8859-1; other characters use its default byte.
	for (size_t index = 0; index < specifiedName.size(); ++index) {
		char16_t character = specifiedName[index];
		name.push_back(character <= 0xff ? static_cast<char>(character) : '?');
		if (character >= 0xd800 && character <= 0xdbff && index + 1 < specifiedName.size() &&
			specifiedName[index + 1] >= 0xdc00 && specifiedName[index + 1] <= 0xdfff)
			++index;
	}
	if (name.size() >= std::numeric_limits<DWORD>::max()) {
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	if (lpBuffer && nBufferLength > name.size()) {
		std::memcpy(lpBuffer, name.c_str(), name.size() + 1);
		return static_cast<DWORD>(name.size());
	}
	if (lpBuffer && nBufferLength)
		lpBuffer[0] = 0;
	return static_cast<DWORD>(name.size() + 1);
}

static BOOL setDllDirectory(std::u16string specifiedName) {
	std::string utf8;
	if (!utf16ToUtf8(specifiedName, utf8)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	auto hostPath = specifiedName.empty() ? std::filesystem::path{} : files::pathFromWindows(utf8.c_str());
	wibo::setDllDirectoryOverride(hostPath, std::move(specifiedName));
	return TRUE;
}

BOOL WINAPI SetDllDirectoryA(LPCSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetDllDirectoryA(%s)\n", lpPathName ? lpPathName : "(null)");
	if (!lpPathName) {
		wibo::clearDllDirectoryOverride();
		return TRUE;
	}
	return setDllDirectory(stringToUtf16(lpPathName));
}

BOOL WINAPI SetDllDirectoryW(LPCWSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetDllDirectoryW(%p)\n", lpPathName);
	if (!lpPathName) {
		wibo::clearDllDirectoryOverride();
		return TRUE;
	}
	return setDllDirectory(std::u16string(reinterpret_cast<const char16_t *>(lpPathName), wstrlen(lpPathName)));
}

BOOL WINAPI FindActCtxSectionStringA(DWORD dwFlags, const GUID *lpExtensionGuid, ULONG ulSectionId,
									 LPCSTR lpStringToFind, PACTCTX_SECTION_KEYED_DATA ReturnedData) {
	DEBUG_LOG("FindActCtxSectionStringA(%#x, %p, %u, %s, %p)\n", dwFlags, lpExtensionGuid, ulSectionId,
			  lpStringToFind ? lpStringToFind : "<null>", ReturnedData);
	std::vector<uint16_t> wideStorage;
	if (lpStringToFind) {
		size_t length = strlen(lpStringToFind);
		wideStorage.resize(length + 1);
		for (size_t i = 0; i <= length; ++i) {
			wideStorage[i] = static_cast<uint8_t>(lpStringToFind[i]);
		}
	}
	const uint16_t *widePtr = wideStorage.empty() ? nullptr : wideStorage.data();
	return FindActCtxSectionStringW(dwFlags, lpExtensionGuid, ulSectionId, reinterpret_cast<LPCWSTR>(widePtr),
									ReturnedData);
}

BOOL WINAPI FindActCtxSectionStringW(DWORD dwFlags, const GUID *lpExtensionGuid, ULONG ulSectionId,
									 LPCWSTR lpStringToFind, PACTCTX_SECTION_KEYED_DATA ReturnedData) {
	std::string lookup = lpStringToFind ? wideStringToString(lpStringToFind) : std::string();
	DEBUG_LOG("FindActCtxSectionStringW(%#x, %p, %u, %s, %p)\n", dwFlags, lpExtensionGuid, ulSectionId, lookup.c_str(),
			  ReturnedData);

	if (lpExtensionGuid) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!ReturnedData) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (dwFlags & ~FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	ULONG originalSize = ReturnedData->cbSize;
	if (originalSize < sizeof(ACTCTX_SECTION_KEYED_DATA)) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	ensureDefaultActivationContext();
	ActivationContext *ctx = currentActivationContext();
	const DllRedirectionEntry *matchedEntry = nullptr;
	if (ulSectionId == ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION && !lookup.empty()) {
		std::string lowerLookup = stringToLower(lookup);
		for (const auto &entry : ctx->dllRedirections) {
			if (entry.nameLower == lowerLookup) {
				matchedEntry = &entry;
				break;
			}
		}
	}

	size_t zeroSize = std::min(static_cast<size_t>(ReturnedData->cbSize), sizeof(*ReturnedData));
	std::memset(ReturnedData, 0, zeroSize);
	ReturnedData->cbSize = originalSize;
	ReturnedData->ulDataFormatVersion = 1;
	ReturnedData->ulFlags = ACTCTX_SECTION_KEYED_DATA_FLAG_FOUND_IN_ACTCTX;
	if (dwFlags & FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX) {
		ReturnedData->hActCtx = static_cast<HANDLE>(toGuestPtr(currentActivationContext()));
	}

	if (!matchedEntry) {
		setLastError(ERROR_SXS_KEY_NOT_FOUND);
		return FALSE;
	}

	ReturnedData->lpData = toGuestPtr(matchedEntry->dllData.get());
	ReturnedData->ulLength = matchedEntry->dllData->Size;
	ReturnedData->lpSectionBase = toGuestPtr(matchedEntry->dllData.get());
	ReturnedData->ulSectionTotalLength = matchedEntry->dllData->Size;
	ReturnedData->ulAssemblyRosterIndex = 1;
	ReturnedData->AssemblyMetadata = {};

	return TRUE;
}

void tryMarkExecutable(void *mem) {
	if (!mem) {
		return;
	}
	size_t usable = mi_usable_size(mem);
	if (usable == 0) {
		return;
	}
	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0) {
		return;
	}
	uintptr_t start = reinterpret_cast<uintptr_t>(mem);
	uintptr_t alignedStart = start & ~static_cast<uintptr_t>(pageSize - 1);
	uintptr_t end = (start + usable + pageSize - 1) & ~static_cast<uintptr_t>(pageSize - 1);
	size_t length = static_cast<size_t>(end - alignedStart);
	if (length == 0) {
		return;
	}
	mprotect(reinterpret_cast<void *>(alignedStart), length, PROT_READ | PROT_WRITE | PROT_EXEC);
}

BOOL WINAPI IsBadCodePtr(FARPROC lpfn) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsBadCodePtr(%p)\n", lpfn);
	// The Windows contract tests read access, not execute permission or code.
	return isBadMemoryRange(lpfn, 1, false) ? TRUE : FALSE;
}

BOOL WINAPI IsBadReadPtr(LPCVOID lp, UINT_PTR ucb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsBadReadPtr(ptr=%p, size=%zu)\n", lp, static_cast<size_t>(ucb));
	return isBadMemoryRange(lp, ucb, false) ? TRUE : FALSE;
}

BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR ucb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsBadWritePtr(ptr=%p, size=%zu)\n", lp, static_cast<size_t>(ucb));
	return isBadMemoryRange(lp, ucb, true) ? TRUE : FALSE;
}

BOOL WINAPI IsBadStringPtrA(LPCSTR lpsz, UINT_PTR ucchMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsBadStringPtrA(ptr=%p, max=%zu)\n", lpsz, static_cast<size_t>(ucchMax));
	return isBadString(lpsz, ucchMax) ? TRUE : FALSE;
}

BOOL WINAPI IsBadStringPtrW(LPCWSTR lpsz, UINT_PTR ucchMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsBadStringPtrW(ptr=%p, max=%zu)\n", lpsz, static_cast<size_t>(ucchMax));
	return isBadString(lpsz, ucchMax) ? TRUE : FALSE;
}

BOOL WINAPI GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetComputerNameA(%p, %p)\n", lpBuffer, nSize);
	if (!nSize || !lpBuffer) {
		if (nSize) {
			*nSize = 0;
		}
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (*nSize < kComputerNameRequiredSize) {
		*nSize = kComputerNameRequiredSize;
		setLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}

	std::strcpy(lpBuffer, kComputerNameAnsi);
	*nSize = kComputerNameLength;
	return TRUE;
}

BOOL WINAPI GetComputerNameW(LPWSTR lpBuffer, LPDWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetComputerNameW(%p, %p)\n", lpBuffer, nSize);
	if (!nSize || !lpBuffer) {
		if (nSize) {
			*nSize = 0;
		}
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (*nSize < kComputerNameRequiredSize) {
		*nSize = kComputerNameRequiredSize;
		setLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}

	wstrncpy(lpBuffer, kComputerNameWide, static_cast<size_t>(kComputerNameRequiredSize));
	*nSize = kComputerNameLength;
	return TRUE;
}

HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalAlloc(%x, %zu)\n", uFlags, static_cast<size_t>(dwBytes));
	if (uFlags & GMEM_MOVEABLE) {
		// not implemented rn
		assert(0);
		return NO_HANDLE;
	}
	bool zero = (uFlags & GMEM_ZEROINIT) != 0;
	void *ret = wibo::heap::guestMalloc(static_cast<UINT>(dwBytes), zero);
	VERBOSE_LOG("-> %p\n", ret);
	if (!ret) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	return toGuestPtr(ret);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalFree(%p)\n", hMem);
	if (wibo::heap::guestFree(reinterpret_cast<void *>(hMem))) {
		VERBOSE_LOG("-> success\n");
		return GUEST_NULL;
	} else {
		VERBOSE_LOG("-> failure\n");
		return hMem;
	}
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes, UINT uFlags) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalReAlloc(%p, %zu, %x)\n", hMem, static_cast<size_t>(dwBytes), uFlags);
	if (uFlags & GMEM_MODIFY) {
		assert(0);
		return GUEST_NULL;
	}
	bool zero = (uFlags & GMEM_ZEROINIT) != 0;
	void *ret = wibo::heap::guestRealloc(reinterpret_cast<void *>(hMem), static_cast<UINT>(dwBytes), zero);
	VERBOSE_LOG("-> %p\n", ret);
	if (!ret) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	return toGuestPtr(ret);
}

UINT WINAPI GlobalFlags(HGLOBAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalFlags(%p)\n", hMem);
	(void)hMem;
	return 0;
}

void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalMemoryStatus(%p)\n", lpBuffer);
	if (!lpBuffer) {
		return;
	}

	std::memset(lpBuffer, 0, sizeof(*lpBuffer));
	lpBuffer->dwLength = sizeof(*lpBuffer);

	MemorySnapshot snapshot;
	if (!queryHostMemory(snapshot)) {
		return;
	}

	uint64_t totalPhys = snapshot.totalPhys;
	uint64_t availPhys = snapshot.availPhys;
	uint64_t totalPageFile = snapshot.totalPageFile;
	uint64_t availPageFile = snapshot.availPageFile;

	constexpr uint64_t kMinAppAddr = 0x00010000ULL;
	constexpr uint64_t kMaxAppAddr = 0x7FFEFFFFULL;
	uint64_t totalVirtual = kMaxAppAddr - kMinAppAddr + 1;
	uint64_t availVirtual = totalVirtual;

	constexpr uint64_t kMaxLegacy = static_cast<uint64_t>(std::numeric_limits<LONG>::max());
	totalPhys = std::min(totalPhys, kMaxLegacy);
	availPhys = std::min(availPhys, kMaxLegacy);
	totalVirtual = std::min(totalVirtual, kMaxLegacy);
	availVirtual = std::min(availVirtual, kMaxLegacy);
	availPhys = std::min(availPhys, totalPhys);
	availPageFile = std::min(availPageFile, totalPageFile);
	availVirtual = std::min(availVirtual, totalVirtual);

	if (totalPhys > 0) {
		uint64_t used = totalPhys > availPhys ? totalPhys - availPhys : 0;
		uint64_t load = (used * 100) / totalPhys;
		lpBuffer->dwMemoryLoad = static_cast<DWORD>(std::min<uint64_t>(load, 100));
	}

	lpBuffer->dwTotalPhys = clampToSizeT(totalPhys);
	lpBuffer->dwAvailPhys = clampToSizeT(availPhys);
	lpBuffer->dwTotalPageFile = clampToSizeT(totalPageFile);
	lpBuffer->dwAvailPageFile = clampToSizeT(availPageFile);
	lpBuffer->dwTotalVirtual = clampToSizeT(totalVirtual);
	lpBuffer->dwAvailVirtual = clampToSizeT(availVirtual);
}

HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalAlloc(%x, %zu)\n", uFlags, static_cast<size_t>(uBytes));
	bool zero = (uFlags & LMEM_ZEROINIT) != 0;
	if ((uFlags & LMEM_MOVEABLE) != 0) {
		VERBOSE_LOG("  ignoring LMEM_MOVEABLE\n");
	}
	void *result = wibo::heap::guestMalloc(static_cast<UINT>(uBytes), zero);
	if (!result) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	// Legacy Windows applications (pre-NX and DEP) may expect executable memory from LocalAlloc.
	tryMarkExecutable(result);
	VERBOSE_LOG("  -> %p\n", result);
	return toGuestPtr(result);
}

HLOCAL WINAPI LocalFree(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalFree(%p)\n", hMem);
	if (wibo::heap::guestFree(reinterpret_cast<void *>(hMem))) {
		VERBOSE_LOG("-> success\n");
		return GUEST_NULL;
	} else {
		VERBOSE_LOG("-> failure\n");
		setLastError(ERROR_INVALID_HANDLE);
		return hMem;
	}
}

HLOCAL WINAPI LocalReAlloc(HLOCAL hMem, SIZE_T uBytes, UINT uFlags) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalReAlloc(%p, %zu, %x)\n", hMem, static_cast<size_t>(uBytes), uFlags);
	bool zero = (uFlags & LMEM_ZEROINIT) != 0;
	if ((uFlags & LMEM_MOVEABLE) != 0) {
		VERBOSE_LOG("  ignoring LMEM_MOVEABLE\n");
	}
	void *result = wibo::heap::guestRealloc(reinterpret_cast<void *>(hMem), static_cast<UINT>(uBytes), zero);
	if (!result && uBytes != 0) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	// Legacy Windows applications (pre-NX and DEP) may expect executable memory from LocalReAlloc.
	tryMarkExecutable(result);
	VERBOSE_LOG("  -> %p\n", result);
	return toGuestPtr(result);
}

HLOCAL WINAPI LocalHandle(LPCVOID pMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalHandle(%p)\n", pMem);
	return toGuestPtr(pMem);
}

LPVOID WINAPI LocalLock(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: LocalLock(%p)\n", hMem);
	return reinterpret_cast<void *>(hMem);
}

BOOL WINAPI LocalUnlock(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalUnlock(%p)\n", hMem);
	(void)hMem;
	return TRUE;
}

SIZE_T WINAPI LocalSize(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalSize(%p)\n", hMem);
	return hMem ? mi_usable_size(reinterpret_cast<void *>(hMem)) : 0;
}

UINT WINAPI LocalFlags(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: LocalFlags(%p)\n", hMem);
	(void)hMem;
	return 0;
}

static constexpr const char *kSystemDirectoryA = "C:\\Windows\\System32";

UINT WINAPI GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemDirectoryA(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	const auto len = std::strlen(kSystemDirectoryA);
	if (uSize < len + 1) {
		return static_cast<UINT>(len + 1);
	}
	std::strcpy(lpBuffer, kSystemDirectoryA);
	return static_cast<UINT>(len);
}

UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemDirectoryW(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	auto wide = stringToWideString(kSystemDirectoryA);
	UINT length = static_cast<UINT>(wide.size() - 1);
	if (uSize < length + 1) {
		return length + 1;
	}
	std::memcpy(lpBuffer, wide.data(), (length + 1) * sizeof(uint16_t));
	return length;
}

UINT WINAPI GetSystemWow64DirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemWow64DirectoryA(%p, %u)\n", lpBuffer, uSize);
	(void)lpBuffer;
	(void)uSize;
	setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

UINT WINAPI GetSystemWow64DirectoryW(LPWSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemWow64DirectoryW(%p, %u)\n", lpBuffer, uSize);
	(void)lpBuffer;
	(void)uSize;
	setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

UINT WINAPI GetWindowsDirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetWindowsDirectoryA(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	const char *windowsDir = "C:\\Windows";
	const auto len = std::strlen(windowsDir);
	if (uSize < len + 1) {
		return static_cast<UINT>(len + 1);
	}
	std::strcpy(lpBuffer, windowsDir);
	return static_cast<UINT>(len);
}

UINT WINAPI GetSystemWindowsDirectoryA(LPSTR lpBuffer, UINT uSize) {
	DEBUG_LOG("GetSystemWindowsDirectoryA(%p, %u)\n", lpBuffer, uSize);
	return GetWindowsDirectoryA(lpBuffer, uSize);
}

UINT WINAPI GetSystemWindowsDirectoryW(LPWSTR lpBuffer, UINT uSize) {
	DEBUG_LOG("GetSystemWindowsDirectoryW(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	const char *windowsDir = "C:\\Windows";
	auto wide = stringToWideString(windowsDir);
	UINT length = static_cast<UINT>(wide.size() - 1);
	if (uSize < length + 1) {
		return length + 1;
	}
	std::memcpy(lpBuffer, wide.data(), (length + 1) * sizeof(uint16_t));
	return length;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentDirectoryA(%u, %p)\n", nBufferLength, lpBuffer);

	std::string path;
	if (!tryGetCurrentDirectoryPath(path)) {
		return 0;
	}

	const DWORD required = static_cast<DWORD>(path.size() + 1);
	if (nBufferLength == 0) {
		return required;
	}
	if (!lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nBufferLength < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return required;
	}
	std::memcpy(lpBuffer, path.c_str(), required);
	return required - 1;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentDirectoryW(%u, %p)\n", nBufferLength, lpBuffer);

	std::string path;
	if (!tryGetCurrentDirectoryPath(path)) {
		return 0;
	}
	auto widePath = stringToWideString(path.c_str());
	const DWORD required = static_cast<DWORD>(widePath.size());
	if (nBufferLength == 0) {
		return required;
	}
	if (!lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nBufferLength < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return required;
	}
	std::copy(widePath.begin(), widePath.end(), lpBuffer);
	return required - 1;
}

int WINAPI SetCurrentDirectoryA(LPCSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetCurrentDirectoryA(%s)\n", lpPathName ? lpPathName : "(null)");
	if (!lpPathName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	auto hostPath = files::pathFromWindows(lpPathName);
	std::error_code ec;
	std::filesystem::current_path(hostPath, ec);
	if (ec) {
		setLastError(wibo::winErrorFromErrno(ec.value()));
		return 0;
	}
	return 1;
}

int WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetCurrentDirectoryW\n");
	if (!lpPathName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string path = wideStringToString(lpPathName);
	return SetCurrentDirectoryA(path.c_str());
}

DWORD WINAPI GetLongPathNameA(LPCSTR lpszShortPath, LPSTR lpszLongPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLongPathNameA(%s, %p, %u)\n", lpszShortPath ? lpszShortPath : "(null)", lpszLongPath, cchBuffer);
	if (!lpszShortPath) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	std::string input(lpszShortPath);
	std::string longPath;
	if (!computeLongWindowsPath(input, longPath)) {
		return 0;
	}

	DWORD required = static_cast<DWORD>(longPath.size() + 1);
	if (cchBuffer == 0) {
		return required;
	}
	if (!lpszLongPath) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (cchBuffer < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return required;
	}
	std::memcpy(lpszLongPath, longPath.c_str(), required);
	return required - 1;
}

DWORD WINAPI GetLongPathNameW(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLongPathNameW(%p, %p, %u)\n", lpszShortPath, lpszLongPath, cchBuffer);
	if (!lpszShortPath) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string input = wideStringToString(lpszShortPath);
	std::string longPath;
	if (!computeLongWindowsPath(input, longPath)) {
		return 0;
	}
	auto wideLong = stringToWideString(longPath.c_str());
	DWORD required = static_cast<DWORD>(wideLong.size());
	if (cchBuffer == 0) {
		return required;
	}
	if (!lpszLongPath) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (cchBuffer < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return required;
	}
	std::copy(wideLong.begin(), wideLong.end(), lpszLongPath);
	return required - 1;
}

BOOL WINAPI GetDiskFreeSpaceA(LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
							  LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceA(%s)\n", lpRootPathName ? lpRootPathName : "(null)");
	struct statvfs buf{};
	std::string resolvedPath;
	if (!resolveDiskFreeSpaceStat(lpRootPathName, buf, resolvedPath)) {
		return FALSE;
	}

	uint64_t blockSize = buf.f_frsize ? buf.f_frsize : buf.f_bsize;
	if (blockSize == 0) {
		blockSize = 4096;
	}
	unsigned int bytesPerSector = 512;
	if (blockSize % bytesPerSector != 0) {
		bytesPerSector =
			static_cast<unsigned int>(std::min<uint64_t>(blockSize, std::numeric_limits<unsigned int>::max()));
	}
	unsigned int sectorsPerCluster = static_cast<unsigned int>(blockSize / bytesPerSector);
	if (sectorsPerCluster == 0) {
		sectorsPerCluster = 1;
		bytesPerSector =
			static_cast<unsigned int>(std::min<uint64_t>(blockSize, std::numeric_limits<unsigned int>::max()));
	}

	uint64_t totalClusters64 = buf.f_blocks;
	uint64_t freeClusters64 = buf.f_bavail;

	if (lpSectorsPerCluster) {
		*lpSectorsPerCluster = sectorsPerCluster;
	}
	if (lpBytesPerSector) {
		*lpBytesPerSector = bytesPerSector;
	}
	if (lpNumberOfFreeClusters) {
		uint64_t clamped = std::min<uint64_t>(freeClusters64, std::numeric_limits<unsigned int>::max());
		*lpNumberOfFreeClusters = static_cast<DWORD>(clamped);
	}
	if (lpTotalNumberOfClusters) {
		uint64_t clamped = std::min<uint64_t>(totalClusters64, std::numeric_limits<unsigned int>::max());
		*lpTotalNumberOfClusters = static_cast<DWORD>(clamped);
	}

	DEBUG_LOG("\t-> host %s, spc %u, bps %u, free clusters %u, total clusters %u\n", resolvedPath.c_str(),
			  lpSectorsPerCluster ? *lpSectorsPerCluster : 0, lpBytesPerSector ? *lpBytesPerSector : 0,
			  lpNumberOfFreeClusters ? *lpNumberOfFreeClusters : 0,
			  lpTotalNumberOfClusters ? *lpTotalNumberOfClusters : 0);
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceW(LPCWSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
							  LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters) {
	HOST_CONTEXT_GUARD();
	std::string rootPath = wideStringToString(lpRootPathName);
	return GetDiskFreeSpaceA(lpRootPathName ? rootPath.c_str() : nullptr, lpSectorsPerCluster, lpBytesPerSector,
							 lpNumberOfFreeClusters, lpTotalNumberOfClusters);
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
								PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceExA(%s)\n", lpDirectoryName ? lpDirectoryName : "(null)");
	struct statvfs buf{};
	std::string resolvedPath;
	if (!resolveDiskFreeSpaceStat(lpDirectoryName, buf, resolvedPath)) {
		return FALSE;
	}

	uint64_t blockSize = buf.f_frsize ? buf.f_frsize : buf.f_bsize;
	if (blockSize == 0) {
		blockSize = 4096;
	}

	uint64_t freeToCaller = static_cast<uint64_t>(buf.f_bavail) * blockSize;
	uint64_t totalBytes = static_cast<uint64_t>(buf.f_blocks) * blockSize;
	uint64_t totalFree = static_cast<uint64_t>(buf.f_bfree) * blockSize;

	if (lpFreeBytesAvailableToCaller) {
		lpFreeBytesAvailableToCaller->QuadPart = freeToCaller;
	}
	if (lpTotalNumberOfBytes) {
		lpTotalNumberOfBytes->QuadPart = totalBytes;
	}
	if (lpTotalNumberOfFreeBytes) {
		lpTotalNumberOfFreeBytes->QuadPart = totalFree;
	}

	DEBUG_LOG("\t-> host %s, free %llu, total %llu, total free %llu\n", resolvedPath.c_str(),
			  static_cast<unsigned long long>(freeToCaller), static_cast<unsigned long long>(totalBytes),
			  static_cast<unsigned long long>(totalFree));
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
								PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceExW -> ");
	std::string directoryName = wideStringToString(lpDirectoryName);
	return GetDiskFreeSpaceExA(lpDirectoryName ? directoryName.c_str() : nullptr, lpFreeBytesAvailableToCaller,
							   lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

} // namespace kernel32
