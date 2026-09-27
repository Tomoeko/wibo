#include "modules.h"

#include "common.h"
#include "entry.h"
#include "entry_trampolines.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "kernel32/errhandlingapi.h"
#include "kernel32/internal.h"
#include "kernel32/processenv.h"
#include "kernel32/winbase.h"
#include "setup.h"
#include "strutil.h"
#include "system_provider.h"
#include "tls.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern const wibo::ModuleStub lib_advapi32;
extern const wibo::ModuleStub lib_bcrypt;
extern const wibo::ModuleStub lib_combase;
extern const wibo::ModuleStub lib_dbghelp;
extern const wibo::ModuleStub lib_kernel32;
extern const wibo::ModuleStub lib_iphlpapi;
extern const wibo::ModuleStub lib_lmgr;
extern const wibo::ModuleStub lib_mscoree;
#if WIBO_HAS_MSVCRT
extern const wibo::ModuleStub lib_msvcrt;
#endif
#if WIBO_HAS_MSVCIRT
extern const wibo::ModuleStub lib_msvcirt;
#endif
#if WIBO_HAS_MSVCRT20
extern const wibo::ModuleStub lib_msvcrt20;
#endif
#if WIBO_HAS_MSVCRT40
extern const wibo::ModuleStub lib_msvcrt40;
#endif
#if WIBO_HAS_MSVCR71
extern const wibo::ModuleStub lib_msvcr71;
#endif
#if WIBO_HAS_MSVCR90
extern const wibo::ModuleStub lib_msvcr90;
#endif
#if WIBO_HAS_MSVCR100
extern const wibo::ModuleStub lib_msvcr100;
#endif
#if WIBO_HAS_MSVCR120
extern const wibo::ModuleStub lib_msvcr120;
#endif
#if WIBO_HAS_UCRTBASE
extern const wibo::ModuleStub lib_ucrtbase;
#endif
extern const wibo::ModuleStub lib_ntdll;
extern const wibo::ModuleStub lib_rpcrt4;
extern const wibo::ModuleStub lib_setupapi;
extern const wibo::ModuleStub lib_ole32;
extern const wibo::ModuleStub lib_oleaut32;
extern const wibo::ModuleStub lib_psapi;
extern const wibo::ModuleStub lib_shlwapi;
extern const wibo::ModuleStub lib_shell32;
extern const wibo::ModuleStub lib_user32;
extern const wibo::ModuleStub lib_vcruntime;
extern const wibo::ModuleStub lib_version;
extern const wibo::ModuleStub lib_winmm;
extern const wibo::ModuleStub lib_ws2;
extern const wibo::ModuleStub lib_wsock32;
extern const wibo::ModuleStub lib_mswsock;

// setup.S
#ifdef WIBO_GUEST_64
template <size_t Index> void __attribute__((ms_abi)) stubThunk() {
#if defined(__APPLE__)
	TEB *teb = enterHostContext();
#endif
	entry::stubBase(Index);
#if defined(__APPLE__)
	enterGuestContext(teb);
#endif
}
#else
template <size_t Index> void stubThunk();
#endif

namespace {

const std::array<std::pair<std::string_view, std::string_view>, 18> kApiSet = {
	std::pair{"api-ms-win-core-crt-l1-1-0.dll", "msvcrt.dll"},
	std::pair{"api-ms-win-core-crt-l2-1-0.dll", "msvcrt.dll"},
	std::pair{"api-ms-win-crt-conio-l1-1-0.dll", "msvcrt.dll"},
	std::pair{"api-ms-win-crt-convert-l1-1-0.dll", "msvcrt.dll"},
	std::pair{"api-ms-win-crt-environment-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-filesystem-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-heap-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-locale-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-math-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-multibyte-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-private-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-process-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-runtime-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-stdio-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-string-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-time-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-crt-utility-l1-1-0.dll", "ucrtbase.dll"},
	std::pair{"api-ms-win-core-synch-l1-2-0.dll", "kernelbase.dll"},
};

constexpr DWORD DLL_PROCESS_DETACH = 0;
constexpr DWORD DLL_PROCESS_ATTACH = 1;
constexpr DWORD DLL_THREAD_ATTACH = 2;
constexpr DWORD DLL_THREAD_DETACH = 3;
constexpr DWORD TLS_PROCESS_ATTACH = DLL_PROCESS_ATTACH;
constexpr DWORD TLS_PROCESS_DETACH = DLL_PROCESS_DETACH;
constexpr DWORD TLS_THREAD_ATTACH = DLL_THREAD_ATTACH;
constexpr DWORD TLS_THREAD_DETACH = DLL_THREAD_DETACH;

struct PEExportDirectory {
	uint32_t characteristics;
	uint32_t timeDateStamp;
	uint16_t majorVersion;
	uint16_t minorVersion;
	uint32_t name;
	uint32_t base;
	uint32_t numberOfFunctions;
	uint32_t numberOfNames;
	uint32_t addressOfFunctions;
	uint32_t addressOfNames;
	uint32_t addressOfNameOrdinals;
};

#ifdef WIBO_GUEST_64
using StubFuncType = void(__attribute__((ms_abi)) *)();
#else
using StubFuncType = void (*)();
#endif
#ifdef WIBO_GUEST_64
constexpr size_t MAX_STUBS = 0x1000;
#else
constexpr size_t MAX_STUBS = 0x100;
#endif
size_t stubIndex = 0;
std::array<std::string, MAX_STUBS> stubDlls;
std::array<std::string, MAX_STUBS> stubFuncNames;
std::unordered_map<std::string, StubFuncType> stubCache;
std::unordered_map<HANDLE, std::shared_ptr<wibo::ModuleInfo>> g_modules;
HANDLE g_nextStubHandle = 1;

// Windows serializes loader notifications, but the data-structure lock that
// protects the module maps must never be held while guest TLS callbacks or
// DllMain execute. Guest code is allowed to query and recursively load modules.
std::recursive_mutex g_loaderNotificationMutex;
uint64_t g_initializationOrder = 0;
bool g_collectingModules = false;
std::vector<wibo::ModulePtr> g_failedModuleLeases;

std::string makeStubKey(const char *dllName, const char *funcName) {
	std::string key;
	if (dllName) {
		key.assign(dllName);
		toLowerInPlace(key);
	}
	key.push_back(':');
	if (funcName) {
		std::string func(funcName);
		toLowerInPlace(func);
		key += func;
	}
	return key;
}

template <size_t... Indices>
constexpr std::array<StubFuncType, sizeof...(Indices)> makeStubTable(std::index_sequence<Indices...>) {
	return {{stubThunk<Indices>...}};
}

constexpr auto stubFuncs = makeStubTable(std::make_index_sequence<MAX_STUBS>{});

StubFuncType resolveMissingFuncName(const char *dllName, const char *funcName) {
	DEBUG_LOG("Missing function: %s (%s)\n", dllName, funcName);
	std::string key = makeStubKey(dllName, funcName);
	auto existing = stubCache.find(key);
	if (existing != stubCache.end()) {
		return existing->second;
	}
	if (stubIndex >= MAX_STUBS) {
		fprintf(stderr, "wibo: too many missing functions encountered (>%zu). Last failure: %s (%s)\n", MAX_STUBS,
				funcName, dllName);
		fflush(stderr);
#if defined(__APPLE__)
		wibo::uninstallTebForCurrentThread();
		_exit(127);
#else
		abort();
#endif
	}
	stubFuncNames[stubIndex] = funcName ? funcName : "";
	stubDlls[stubIndex] = dllName ? dllName : "";
	StubFuncType stub = stubFuncs[stubIndex];
	stubCache.emplace(std::move(key), stub);
	stubIndex++;
	return stub;
}

StubFuncType resolveMissingFuncOrdinal(const char *dllName, uint16_t ordinal) {
	std::string funcName = std::to_string(ordinal);
	return resolveMissingFuncName(dllName, funcName.c_str());
}

struct ForwarderLookup {
	std::vector<std::pair<wibo::ModuleInfo *, uint16_t>> path;
	bool cycleDetected = false;
	bool staticImport = false;
};

void *findExportByNameInternal(wibo::ModuleInfo *info, const char *funcName, const wibo::ModuleSearch &search,
							   wibo::ModuleInfo *importer, ForwarderLookup &lookup);
void *findExportByOrdinalInternal(wibo::ModuleInfo *info, uint16_t ordinal, const wibo::ModuleSearch &search,
								  wibo::ModuleInfo *importer, ForwarderLookup &lookup);

wibo::ModuleInfo *loadForwarderTargetModule(std::string &dllName, const wibo::ModuleSearch &search,
											wibo::ModuleInfo &importer) {
	wibo::ModuleInfo *target = wibo::loadDependency(importer, dllName.c_str(), search);
	if (target || dllName.empty() || dllName[0] != '_') {
		return target;
	}

	std::string undecoratedName = dllName.substr(1);
	target = wibo::loadDependency(importer, undecoratedName.c_str(), search);
	if (target) {
		DEBUG_LOG("Forwarded export: treating decorated DLL name %s as %s\n", dllName.c_str(), undecoratedName.c_str());
		dllName = std::move(undecoratedName);
	}
	return target;
}

void *resolveForwardedExport(wibo::ModuleInfo &source, const char *forwarder, const wibo::ModuleSearch &search,
							 wibo::ModuleInfo &importer, ForwarderLookup &lookup) {
	auto missing = [&](const char *dllName, const char *exportName) -> void * {
		return lookup.staticImport ? reinterpret_cast<void *>(resolveMissingFuncName(dllName, exportName)) : nullptr;
	};
	if (!forwarder || !*forwarder) {
		return missing(source.originalName.c_str(), "");
	}

	const char *separator = std::strchr(forwarder, '.');
	if (!separator || separator == forwarder || !separator[1]) {
		return missing(source.originalName.c_str(), forwarder);
	}

	std::string dllName(forwarder, separator - forwarder);
	std::string exportName(separator + 1);
	DEBUG_LOG("Forwarded export: %s!%s -> %s!%s\n", source.originalName.c_str(), forwarder, dllName.c_str(),
			  exportName.c_str());

	wibo::ModuleInfo *target = loadForwarderTargetModule(dllName, search, importer);
	if (!target) {
		return missing(dllName.c_str(), exportName.c_str());
	}

	if (exportName[0] == '#') {
		char *end = nullptr;
		unsigned long ordinal = std::strtoul(exportName.c_str() + 1, &end, 10);
		if (end && *end == '\0' && ordinal <= UINT16_MAX) {
			void *func = findExportByOrdinalInternal(target, static_cast<uint16_t>(ordinal), search, &importer, lookup);
			if (func) {
				return func;
			}
		}
		return lookup.cycleDetected ? nullptr : missing(dllName.c_str(), exportName.c_str());
	}

	void *func = findExportByNameInternal(target, exportName.c_str(), search, &importer, lookup);
	if (func) {
		return func;
	}
	return lookup.cycleDetected ? nullptr : missing(dllName.c_str(), exportName.c_str());
}

struct ModuleRegistry {
	std::recursive_mutex mutex;
	std::unordered_map<std::string, wibo::ModulePtr> modulesByKey;
	std::unordered_map<std::string, wibo::ModuleInfo *> modulesByAlias;
	std::optional<std::filesystem::path> dllDirectory;
	std::u16string dllDirectorySpecifiedName;
	bool initialized = false;
	std::unordered_map<const wibo::ModuleStub *, std::vector<std::string>> builtinAliasLists;
	std::unordered_map<std::string, wibo::ModuleInfo *> builtinAliasMap;
	std::unordered_set<std::string> pinnedAliases;
	std::unordered_set<wibo::ModuleInfo *> pinnedModules;
	std::vector<wibo::ModuleInfo *> tlsModuleSlots;
};

struct LockedRegistry {
	ModuleRegistry *reg;
	std::unique_lock<std::recursive_mutex> lock;

	LockedRegistry(ModuleRegistry &registryRef, std::unique_lock<std::recursive_mutex> &&guard)
		: reg(&registryRef), lock(std::move(guard)) {}

	LockedRegistry(const LockedRegistry &) = delete;
	LockedRegistry &operator=(const LockedRegistry &) = delete;
	LockedRegistry(LockedRegistry &&) = default;
	LockedRegistry &operator=(LockedRegistry &&) = default;

	[[nodiscard]] ModuleRegistry &get() const { return *reg; }
	ModuleRegistry *operator->() const { return reg; }
	ModuleRegistry &operator*() const { return *reg; }
};

void registerBuiltinModule(ModuleRegistry &reg, const wibo::ModuleStub *module);
bool shouldDeliverThreadNotifications(const wibo::ModuleInfo &info);

using ThreadNotificationSnapshot = std::shared_ptr<const std::vector<wibo::ModulePtr>>;
ThreadNotificationSnapshot g_threadNotificationSnapshot;

void publishThreadNotificationSnapshot(ModuleRegistry &reg) {
	auto targets = std::make_shared<std::vector<wibo::ModulePtr>>();
	targets->reserve(reg.modulesByKey.size());
	for (auto &pair : reg.modulesByKey) {
		if (pair.second && shouldDeliverThreadNotifications(*pair.second)) {
			targets->push_back(pair.second);
		}
	}
	g_threadNotificationSnapshot = std::move(targets);
}

LockedRegistry registry() {
	static ModuleRegistry reg;
	std::unique_lock guard(reg.mutex);
	if (!reg.initialized) {
		reg.initialized = true;
		// clang-format off
		const wibo::ModuleStub *builtins[] = {
			&lib_advapi32,
			&lib_bcrypt,
			&lib_combase,
			&lib_dbghelp,
			&lib_kernel32,
			&lib_iphlpapi,
			&lib_lmgr,
			&lib_mscoree,
			&lib_ntdll,
			&lib_ole32,
			&lib_oleaut32,
			&lib_psapi,
			&lib_rpcrt4,
			&lib_setupapi,
			&lib_shlwapi,
			&lib_shell32,
			&lib_user32,
			&lib_vcruntime,
			&lib_version,
			&lib_winmm,
			&lib_ws2,
			&lib_wsock32,
			&lib_mswsock,
#if WIBO_HAS_MSVCRT
			&lib_msvcrt,
#endif
#if WIBO_HAS_MSVCIRT
			&lib_msvcirt,
#endif
#if WIBO_HAS_MSVCRT20
			&lib_msvcrt20,
#endif
#if WIBO_HAS_MSVCRT40
			&lib_msvcrt40,
#endif
#if WIBO_HAS_MSVCR71
			&lib_msvcr71,
#endif
#if WIBO_HAS_MSVCR90
			&lib_msvcr90,
#endif
#if WIBO_HAS_MSVCR100
			&lib_msvcr100,
#endif
#if WIBO_HAS_MSVCR120
			&lib_msvcr120,
#endif
#if WIBO_HAS_UCRTBASE
			&lib_ucrtbase,
#endif
			nullptr,
		};
		// clang-format on
		for (const wibo::ModuleStub **module = builtins; *module; ++module) {
			registerBuiltinModule(reg, *module);
		}
	}
	return {reg, std::move(guard)};
}

ThreadNotificationSnapshot snapshotThreadNotificationModules() { return g_threadNotificationSnapshot; }

DWORD allocateModuleTlsSlot(ModuleRegistry &reg, wibo::ModuleInfo &module) {
	for (DWORD i = 0; i < static_cast<DWORD>(reg.tlsModuleSlots.size()); ++i) {
		if (reg.tlsModuleSlots[i] == nullptr) {
			reg.tlsModuleSlots[i] = &module;
			return i;
		}
	}
	reg.tlsModuleSlots.push_back(&module);
	return static_cast<DWORD>(reg.tlsModuleSlots.size() - 1);
}

void releaseModuleTlsSlot(ModuleRegistry &reg, DWORD index) {
	if (index < static_cast<DWORD>(reg.tlsModuleSlots.size())) {
		reg.tlsModuleSlots[index] = nullptr;
	}
}

std::string normalizeAlias(const std::string &value) {
	std::string out = value;
	std::replace(out.begin(), out.end(), '/', '\\');
	toLowerInPlace(out);
	return out;
}

struct ParsedModuleName {
	std::string original;
	std::string directory; // Windows-style directory component (may be empty)
	std::string base;
	bool hasExtension = false;
	bool endsWithDot = false;
};

ParsedModuleName parseModuleName(std::string_view name) {
	ParsedModuleName parsed;
	parsed.original = name;
	parsed.base = name;
	std::string sanitized{name};
	std::replace(sanitized.begin(), sanitized.end(), '/', '\\');
	auto sep = sanitized.find_last_of('\\');
	if (sep != std::string::npos) {
		parsed.directory = sanitized.substr(0, sep);
		parsed.base = sanitized.substr(sep + 1);
	} else {
		parsed.base = sanitized;
	}
	parsed.endsWithDot = !parsed.base.empty() && parsed.base.back() == '.';
	parsed.hasExtension = (!parsed.endsWithDot) && parsed.base.find('.') != std::string::npos;
	return parsed;
}

std::vector<std::string> candidateModuleNames(const ParsedModuleName &parsed) {
	std::vector<std::string> names;
	if (!parsed.base.empty()) {
		names.push_back(parsed.base);
		if (!parsed.hasExtension && !parsed.endsWithDot) {
			names.push_back(parsed.base + ".dll");
		}
	}
	return names;
}

std::string normalizedBaseKey(const ParsedModuleName &parsed) {
	if (parsed.base.empty()) {
		return {};
	}
	std::string base = parsed.base;
	if (!parsed.hasExtension && !parsed.endsWithDot) {
		base += ".dll";
	}
	return normalizeAlias(base);
}

std::string builtinPreferenceKey(std::string_view name) {
	std::string key(name);
	for (char &value : key)
		if (value >= 'A' && value <= 'Z')
			value += 'a' - 'A';
	if (key.ends_with(".dll"))
		key.resize(key.size() - 4);
	return key;
}

const std::unordered_set<std::string> &configuredBuiltinNames() {
	// Snapshot host startup preferences; guest environment changes cannot select modules.
	static const auto names = [] {
		std::unordered_set<std::string> selected;
		const char *setting = std::getenv("WIBO_BUILTIN_MODULES");
		if (!setting)
			return selected;
		std::string_view remaining(setting);
		const auto isSpace = [](char value) {
			return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
		};
		while (!remaining.empty()) {
			const size_t separator = remaining.find(';');
			auto token = remaining.substr(0, separator);
			if (separator == std::string_view::npos)
				remaining = {};
			else
				remaining.remove_prefix(separator + 1);
			while (!token.empty() && isSpace(token.front()))
				token.remove_prefix(1);
			while (!token.empty() && isSpace(token.back()))
				token.remove_suffix(1);
			// Only literal ASCII basenames are accepted, without paths or wildcard patterns.
			const bool valid = !token.empty() && std::all_of(token.begin(), token.end(), [](char value) {
				return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
					   (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.';
			});
			if (valid)
				selected.insert(builtinPreferenceKey(token));
		}
		return selected;
	}();
	return names;
}

bool hasConfiguredBuiltinPreference(const wibo::ModuleStub &module) {
	const auto &selected = configuredBuiltinNames();
	for (size_t index = 0; module.names[index]; ++index)
		if (selected.contains(builtinPreferenceKey(module.names[index])))
			return true;
	return false;
}

struct ImageTlsDirectory {
	GUEST_PTR StartAddressOfRawData;
	GUEST_PTR EndAddressOfRawData;
	GUEST_PTR AddressOfIndex;
	GUEST_PTR AddressOfCallBacks;
	uint32_t SizeOfZeroFill;
	uint32_t Characteristics;
};

constexpr size_t kMinTlsDirectorySize = offsetof(ImageTlsDirectory, SizeOfZeroFill);

uintptr_t resolveModuleAddress(const wibo::Executable &exec, uintptr_t address) {
	if (address == 0) {
		return 0;
	}
	const uintptr_t actualBase = reinterpret_cast<uintptr_t>(exec.imageBase);
	if (address >= actualBase) {
		uintptr_t offset = address - actualBase;
		if (offset < exec.imageSize) {
			return address;
		}
	}
	const uintptr_t preferredBase = static_cast<uintptr_t>(exec.preferredImageBase);
	if (address >= preferredBase) {
		return actualBase + (address - preferredBase);
	}
	return static_cast<uintptr_t>(static_cast<intptr_t>(address) + exec.relocationDelta);
}

bool allocateModuleTlsForThread(wibo::ModuleInfo &module, TEB *tib) {
	if (!tib) {
		return true;
	}
	auto &info = module.tlsInfo;
	if (!info.hasTls || info.index == wibo::tls::kInvalidTlsIndex) {
		return true;
	}
	if (info.threadAllocations.find(tib) != info.threadAllocations.end()) {
		return true;
	}
	GUEST_PTR block = GUEST_NULL;
	const size_t allocationSize = info.allocationSize;
	if (allocationSize > 0) {
		void *ptr = wibo::heap::guestMalloc(allocationSize);
		if (!ptr) {
			DEBUG_LOG("  allocateModuleTlsForThread: failed to allocate %zu bytes for %s\n", allocationSize,
					  module.originalName.c_str());
			return false;
		}
		std::memset(ptr, 0, allocationSize);
		if (info.templateData && info.templateSize > 0) {
			std::memcpy(ptr, info.templateData, info.templateSize);
		}
		block = toGuestPtr(ptr);
	}
	info.threadAllocations.emplace(tib, block);
	const bool apiSlotPublished = wibo::tls::setValue(tib, info.index, block);
	if (!apiSlotPublished) {
		DEBUG_LOG("  allocateModuleTlsForThread: failed to publish TLS pointer for %s (index %u)\n",
				  module.originalName.c_str(), info.index);
	}
	bool loaderSlotPublished = true;
	if (info.loaderIndex != wibo::tls::kInvalidTlsIndex) {
		loaderSlotPublished = wibo::tls::setModulePointer(tib, info.loaderIndex, block);
		if (!loaderSlotPublished) {
			DEBUG_LOG("  allocateModuleTlsForThread: failed to update module pointer for %s (slot %u)\n",
					  module.originalName.c_str(), info.loaderIndex);
		}
	}
	if (!apiSlotPublished || !loaderSlotPublished) {
		if (apiSlotPublished) {
			wibo::tls::setValue(tib, info.index, GUEST_NULL);
		}
		if (loaderSlotPublished && info.loaderIndex != wibo::tls::kInvalidTlsIndex) {
			wibo::tls::clearModulePointer(tib, info.loaderIndex);
		}
		info.threadAllocations.erase(tib);
		if (block) {
			wibo::heap::guestFree(fromGuestPtr(block));
		}
		return false;
	}
	return true;
}

void freeModuleTlsForThread(wibo::ModuleInfo &module, TEB *tib) {
	if (!tib) {
		return;
	}
	auto &info = module.tlsInfo;
	if (!info.hasTls) {
		return;
	}
	auto it = info.threadAllocations.find(tib);
	if (it == info.threadAllocations.end()) {
		return;
	}
	GUEST_PTR block = it->second;
	info.threadAllocations.erase(it);
	if (wibo::tls::getValue(tib, info.index) == block) {
		if (!wibo::tls::setValue(tib, info.index, GUEST_NULL)) {
			DEBUG_LOG("  freeModuleTlsForThread: failed to clear TLS pointer for %s (index %u)\n",
					  module.originalName.c_str(), info.index);
		}
	}
	if (info.loaderIndex != wibo::tls::kInvalidTlsIndex) {
		wibo::tls::clearModulePointer(tib, info.loaderIndex);
	}
	if (block) {
		wibo::heap::guestFree(fromGuestPtr(block));
	}
}

void runModuleTlsCallbacks(wibo::ModuleInfo &module, DWORD reason) {
	if (!module.tlsInfo.hasTls || module.tlsInfo.callbacks.empty()) {
		return;
	}
	for (auto *callback : module.tlsInfo.callbacks) {
		if (!callback) {
			continue;
		}
		call_PIMAGE_TLS_CALLBACK(callback, reinterpret_cast<PVOID>(module.handle), reason, nullptr);
	}
}

std::optional<std::filesystem::path> combineAndFind(const std::filesystem::path &directory,
													const std::string &filename) {
	if (filename.empty()) {
		return std::nullopt;
	}
	if (directory.empty()) {
		return std::nullopt;
	}
	return files::findCaseInsensitiveFile(directory, filename);
}

std::vector<std::filesystem::path> collectSearchDirectories(ModuleRegistry &reg, DWORD flags,
															const std::filesystem::path &topDirectory = {}) {
	std::vector<std::filesystem::path> dirs;
	std::unordered_set<std::string> seen;

	auto addDirectory = [&](const std::filesystem::path &dir) {
		if (dir.empty())
			return;
		std::error_code ec;
		auto canonical = std::filesystem::weakly_canonical(dir, ec);
		if (ec) {
			canonical = std::filesystem::absolute(dir, ec);
		}
		if (ec)
			return;
		if (!std::filesystem::exists(canonical, ec) || ec)
			return;
		std::string key = stringToLower(canonical.string());
		if (seen.insert(key).second) {
			dirs.push_back(canonical);
		}
	};

	const auto applicationDirectory = wibo::guestExecutablePath.parent_path();
	if (flags & wibo::ModuleSearch::DefaultDirectories)
		flags |= wibo::ModuleSearch::ApplicationDirectory | wibo::ModuleSearch::UserDirectories |
				 wibo::ModuleSearch::SystemDirectory;
	if (flags & wibo::ModuleSearch::DirectoryMask) {
		if (flags & wibo::ModuleSearch::DllDirectory)
			addDirectory(topDirectory);
		if (flags & wibo::ModuleSearch::ApplicationDirectory)
			addDirectory(applicationDirectory);
		if ((flags & wibo::ModuleSearch::UserDirectories) && reg.dllDirectory)
			addDirectory(*reg.dllDirectory);
		if (flags & wibo::ModuleSearch::SystemDirectory)
			addDirectory(files::systemSearchDirectories().system);
		return dirs;
	}
	addDirectory((flags & wibo::ModuleSearch::AlteredPath) && !topDirectory.empty() ? topDirectory
																					: applicationDirectory);

	if (reg.dllDirectory.has_value()) {
		addDirectory(*reg.dllDirectory);
	}

	if (flags & wibo::ModuleSearch::AlteredPath) {
		const auto system = files::systemSearchDirectories();
		addDirectory(system.system);
		addDirectory(system.legacySystem);
		addDirectory(system.windows);
	}
	if (!reg.dllDirectory.has_value()) {
		addDirectory(std::filesystem::current_path());
	}
	if (flags & wibo::ModuleSearch::AlteredPath) {
		DWORD length = kernel32::GetEnvironmentVariableA("PATH", nullptr, 0);
		if (length) {
			std::vector<char> buffer(length);
			length = kernel32::GetEnvironmentVariableA("PATH", buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length && length < buffer.size()) {
				std::string_view pathList(buffer.data(), length);
				for (size_t start = 0; start < pathList.size();) {
					size_t end = pathList.find(';', start);
					if (end == std::string_view::npos)
						end = pathList.size();
					std::string directory(pathList.substr(start, end - start));
					addDirectory(files::pathFromWindows(directory.c_str()));
					start = end + 1;
				}
			}
		}
		return dirs;
	}

	const auto addFromEnv = [&](const char *envVar) {
		if (const char *envPath = std::getenv(envVar)) {
			std::string pathList = envPath;
			size_t start = 0;
			while (start <= pathList.size()) {
				size_t end = pathList.find_first_of(":;", start);
				if (end == std::string::npos) {
					end = pathList.size();
				}
				if (end > start) {
					auto piece = pathList.substr(start, end - start);
					if (!piece.empty()) {
						auto candidate = files::pathFromWindows(piece.c_str());
						if (!candidate.empty()) {
							addDirectory(candidate);
						} else {
							addDirectory(std::filesystem::path(piece));
						}
					}
				}
				if (end == pathList.size()) {
					break;
				}
				start = end + 1;
			}
		}
	};

	addFromEnv("WIBO_PATH");
	addFromEnv("WINEPATH"); // Wine compatibility

	return dirs;
}

std::optional<std::filesystem::path> resolveModuleOnDisk(ModuleRegistry &reg, const std::string &requestedName,
														 const wibo::ModuleSearch &search) {
	ParsedModuleName parsed = parseModuleName(requestedName);
	auto names = candidateModuleNames(parsed);
	auto directories = search.directoriesResolved ? search.directories : collectSearchDirectories(reg, search.flags);

	if (!parsed.directory.empty()) {
		for (const auto &candidate : names) {
			auto combined = parsed.directory + "\\" + candidate;
			const bool absolute =
				combined.front() == '\\' || (combined.size() > 2 && combined[1] == ':' && combined[2] == '\\');
			if (search.flags && !absolute) {
				for (const auto &directory : directories) {
					auto relative = combined;
					std::replace(relative.begin(), relative.end(), '\\', '/');
					auto windowsPath = files::pathToWindows(directory / relative);
					auto path = files::pathFromWindows(windowsPath.c_str());
					if (auto resolved = files::findCaseInsensitiveFile(path.parent_path(), path.filename().string()))
						return files::canonicalPath(*resolved);
				}
				continue;
			}
			auto posixPath = files::pathFromWindows(combined.c_str());
			if (!posixPath.empty()) {
				auto resolved = files::findCaseInsensitiveFile(std::filesystem::path(posixPath).parent_path(),
															   std::filesystem::path(posixPath).filename().string());
				if (resolved) {
					return files::canonicalPath(*resolved);
				}
			}
		}
		return std::nullopt;
	}

	for (const auto &dir : directories) {
		for (const auto &candidate : names) {
			auto resolved = combineAndFind(dir, candidate);
			if (resolved) {
				return files::canonicalPath(*resolved);
			}
		}
	}

	return std::nullopt;
}

std::string storageKeyForPath(const std::filesystem::path &path) {
	return normalizeAlias(files::pathToWindows(files::canonicalPath(path)));
}

std::string storageKeyForBuiltin(const std::string &normalizedName) { return normalizedName; }

wibo::ModuleInfo *findByAlias(ModuleRegistry &reg, const std::string &alias) {
	auto it = reg.modulesByAlias.find(alias);
	if (it != reg.modulesByAlias.end()) {
		return it->second;
	}
	return nullptr;
}

void registerAlias(ModuleRegistry &reg, const std::string &alias, wibo::ModuleInfo *info) {
	if (alias.empty() || !info) {
		return;
	}
	auto it = reg.modulesByAlias.find(alias);
	if (it == reg.modulesByAlias.end()) {
		reg.modulesByAlias[alias] = info;
		return;
	}
	if (reg.pinnedAliases.count(alias)) {
		return;
	}
	// Prefer externally loaded modules over built-ins when both are present.
	if (it->second && it->second->moduleStub != nullptr && info->moduleStub == nullptr) {
		reg.modulesByAlias[alias] = info;
	}
}

void registerBuiltinModule(ModuleRegistry &reg, const wibo::ModuleStub *module) {
	if (!module) {
		return;
	}

	wibo::ModulePtr entry = std::make_shared<wibo::ModuleInfo>();
	HANDLE handle = g_nextStubHandle++;
	g_modules[handle] = entry;
	entry->handle = handle;
	entry->moduleStub = module;
	entry->executable = nullptr;
	entry->refCount = UINT_MAX;
	entry->originalName = module->names[0] ? module->names[0] : "";
	entry->normalizedName = normalizedBaseKey(parseModuleName(entry->originalName));
	entry->exportsInitialized = false;
	auto storageKey = storageKeyForBuiltin(entry->normalizedName);
	auto raw = entry.get();
	reg.modulesByKey[storageKey] = std::move(entry);

	reg.builtinAliasLists[module] = {};
	auto &aliasList = reg.builtinAliasLists[module];
	// Core runtime aliases must retain the implementations that own guest state.
	const bool pinModule = module == &lib_kernel32 || module == &lib_ntdll || module == &lib_lmgr ||
						   hasConfiguredBuiltinPreference(*module);
	if (pinModule) {
		reg.pinnedModules.insert(raw);
	}
	for (size_t i = 0; module->names[i]; ++i) {
		std::string alias = normalizeAlias(module->names[i]);
		aliasList.push_back(alias);
		if (pinModule) {
			reg.pinnedAliases.insert(alias);
		}
		registerAlias(reg, alias, raw);
		reg.builtinAliasMap[alias] = raw;
		ParsedModuleName parsed = parseModuleName(module->names[i]);
		std::string baseAlias = normalizedBaseKey(parsed);
		if (baseAlias != alias) {
			aliasList.push_back(baseAlias);
			if (pinModule) {
				reg.pinnedAliases.insert(baseAlias);
			}
			registerAlias(reg, baseAlias, raw);
			reg.builtinAliasMap[baseAlias] = raw;
		}
	}
}

BOOL callDllMain(wibo::ModuleInfo &info, DWORD reason, LPVOID reserved) {
	if (&info == wibo::mainModule) {
		return TRUE;
	}
	if (!info.executable) {
		return TRUE;
	}
	void *entry = info.executable->entryPoint;
	if (!entry) {
		return TRUE;
	}

	// Reset last error
	kernel32::setLastError(ERROR_SUCCESS);

	auto dllMain = reinterpret_cast<DllEntryProc>(entry);

	auto invokeWithGuestTIB = [&](DWORD callReason, LPVOID callReserved, bool force) -> BOOL {
		if (!force) {
			if (callReason == DLL_PROCESS_DETACH) {
				if (!info.processAttachCalled || !info.processAttachSucceeded) {
					return TRUE;
				}
			}
			if (callReason == DLL_THREAD_ATTACH || callReason == DLL_THREAD_DETACH) {
				if (!info.processAttachCalled || !info.processAttachSucceeded || !info.threadNotificationsEnabled) {
					return TRUE;
				}
			}
		}

		DEBUG_LOG("  callDllMain: invoking DllMain(%p, %u, %p) for %s\n", toGuestPtr(info.executable->imageBase),
				  callReason, callReserved, info.normalizedName.c_str());

		BOOL result = call_DllEntryProc(dllMain, toGuestPtr(info.executable->imageBase), callReason, callReserved);
		DEBUG_LOG("  callDllMain: %s DllMain returned %d\n", info.normalizedName.c_str(), result);
		return result;
	};

	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		if (info.processAttachCalled) {
			return info.processAttachSucceeded ? TRUE : FALSE;
		}
		info.processAttachCalled = true;
		info.processAttachSucceeded = false;
		BOOL result = invokeWithGuestTIB(DLL_PROCESS_ATTACH, reserved, true);
		if (!result) {
			if (info.tlsInfo.hasTls)
				runModuleTlsCallbacks(info, DLL_PROCESS_DETACH);
			invokeWithGuestTIB(DLL_PROCESS_DETACH, nullptr, true);
			info.detachNotificationsDelivered = true;
			return FALSE;
		}
		info.processAttachSucceeded = true;
		return TRUE;
	}
	case DLL_PROCESS_DETACH: {
		if (!info.processAttachCalled || !info.processAttachSucceeded)
			return TRUE;
		info.processAttachSucceeded = false;
		return invokeWithGuestTIB(DLL_PROCESS_DETACH, reserved, true);
	}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		return invokeWithGuestTIB(reason, reserved, false);
	default:
		break;
	}

	return TRUE;
}

void registerExternalModuleAliases(ModuleRegistry &reg, const std::string &requestedName,
								   const std::filesystem::path &resolvedPath, wibo::ModuleInfo *info) {
	ParsedModuleName parsed = parseModuleName(requestedName);
	registerAlias(reg, normalizedBaseKey(parsed), info);
	registerAlias(reg, normalizeAlias(requestedName), info);
	registerAlias(reg, storageKeyForPath(resolvedPath), info);
}

wibo::ModuleInfo *moduleFromAddress(ModuleRegistry &reg, void *addr) {
	if (!addr)
		return nullptr;
	for (auto &pair : reg.modulesByKey) {
		wibo::ModuleInfo *info = pair.second.get();
		if (!info || !info->executable)
			continue;
		const auto *base = static_cast<const uint8_t *>(info->executable->imageBase);
		size_t size = info->executable->imageSize;
		if (!base || size == 0)
			continue;
		const auto *ptr = static_cast<const uint8_t *>(addr);
		if (ptr >= base && ptr < base + size) {
			return info;
		}
	}
	return nullptr;
}

bool shouldDeliverThreadNotifications(const wibo::ModuleInfo &info) {
	if (&info == wibo::mainModule) {
		return false;
	}
	if (!info.executable) {
		return false;
	}
	if (!info.processAttachCalled || !info.processAttachSucceeded || info.detachNotificationsDelivered) {
		return false;
	}
	if (!info.threadNotificationsEnabled) {
		return false;
	}
	return true;
}

wibo::ModulePtr unregisterModule(ModuleRegistry &reg, wibo::ModuleInfo *info) {
	wibo::ModulePtr owner;
	for (auto it = reg.modulesByKey.begin(); it != reg.modulesByKey.end(); ++it) {
		if (it->second.get() == info) {
			owner = it->second;
			reg.modulesByKey.erase(it);
			break;
		}
	}
	for (auto it = reg.modulesByAlias.begin(); it != reg.modulesByAlias.end();) {
		if (it->second != info) {
			++it;
			continue;
		}
		auto builtin = reg.builtinAliasMap.find(it->first);
		if (builtin != reg.builtinAliasMap.end() && builtin->second != info) {
			it->second = builtin->second;
			++it;
		} else {
			it = reg.modulesByAlias.erase(it);
		}
	}
	for (auto &[key, module] : reg.modulesByKey) {
		std::erase(module->dependencies, info);
	}
	reg.pinnedModules.erase(info);
	publishThreadNotificationSnapshot(reg);
	return owner;
}

std::unordered_set<wibo::ModuleInfo *> reachableModules(ModuleRegistry &reg) {
	std::unordered_set<wibo::ModuleInfo *> reachable;
	std::vector<wibo::ModuleInfo *> pending;
	for (auto &[key, module] : reg.modulesByKey) {
		if (module->refCount || module->loadInProgress || module->moduleStub || module.get() == wibo::mainModule)
			pending.push_back(module.get());
	}
	while (!pending.empty()) {
		auto *module = pending.back();
		pending.pop_back();
		if (reachable.insert(module).second)
			pending.insert(pending.end(), module->dependencies.begin(), module->dependencies.end());
	}
	return reachable;
}

void collectUnusedModules() {
	if (g_collectingModules)
		return;
	g_collectingModules = true;
	std::unordered_set<wibo::ModuleInfo *> notified;
	for (;;) {
		wibo::ModulePtr candidate;
		{
			auto reg = registry();
			auto reachable = reachableModules(*reg);
			for (auto &[key, module] : reg->modulesByKey) {
				if (!reachable.contains(module.get()) && !notified.contains(module.get()) &&
					(!candidate || module->initializationOrder > candidate->initializationOrder))
					candidate = module;
			}
		}
		if (!candidate)
			break;
		notified.insert(candidate.get());
		candidate->dependencies.clear();
		if (!candidate->detachNotificationsDelivered) {
			candidate->detachNotificationsDelivered = true;
			if (candidate->tlsInfo.hasTls)
				runModuleTlsCallbacks(*candidate, DLL_PROCESS_DETACH);
			callDllMain(*candidate, DLL_PROCESS_DETACH, nullptr);
		}
	}

	// Keep every image and TLS block available until the last detach notification returns.
	std::vector<wibo::ModulePtr> retired;
	{
		auto reg = registry();
		auto reachable = reachableModules(*reg);
		for (auto *module : notified) {
			if (!reachable.contains(module))
				retired.push_back(unregisterModule(*reg, module));
		}
		publishThreadNotificationSnapshot(*reg);
	}
	retired.insert(retired.end(), g_failedModuleLeases.begin(), g_failedModuleLeases.end());
	g_failedModuleLeases.clear();
	for (auto &module : retired) {
		releaseModuleTls(*module);
		module->dependencies.clear();
		g_modules.erase(module->handle);
	}
	g_collectingModules = false;
}

void ensureExportsInitialized(wibo::ModuleInfo &info) {
	if (info.exportsInitialized || !info.executable) {
		return;
	}
	auto *exe = info.executable.get();
	if (!exe->exportDirectoryRVA || !exe->exportDirectorySize) {
		info.exportsInitialized = true;
		return;
	}

	auto *dir = exe->fromRVA<PEExportDirectory>(exe->exportDirectoryRVA);
	info.exportOrdinalBase = dir->base;
	uint32_t functionCount = dir->numberOfFunctions;
	info.exportsByOrdinal.assign(functionCount, nullptr);
	if (functionCount) {
		auto *functions = exe->fromRVA<uint32_t>(dir->addressOfFunctions);
		for (uint32_t i = 0; i < functionCount; ++i) {
			uint32_t rva = functions[i];
			if (!rva) {
				continue;
			}
			if (rva >= exe->exportDirectoryRVA && rva < exe->exportDirectoryRVA + exe->exportDirectorySize) {
				const char *forward = exe->fromRVA<const char>(rva);
				info.exportForwarders.emplace(i, forward);
			} else {
				info.exportsByOrdinal[i] = exe->fromRVA<void>(rva);
			}
		}
	}

	uint32_t nameCount = dir->numberOfNames;
	if (nameCount) {
		auto *names = exe->fromRVA<uint32_t>(dir->addressOfNames);
		auto *ordinals = exe->fromRVA<uint16_t>(dir->addressOfNameOrdinals);
		for (uint32_t i = 0; i < nameCount; ++i) {
			uint16_t index = ordinals[i];
			auto ordinal = static_cast<uint16_t>(dir->base + index);
			if (index < info.exportsByOrdinal.size()) {
				const char *namePtr = exe->fromRVA<const char>(names[i]);
				info.exportNameToOrdinal[std::string(namePtr)] = ordinal;
			}
		}
	}
	info.exportsInitialized = true;
}

void *findExportByNameInternal(wibo::ModuleInfo *info, const char *funcName, const wibo::ModuleSearch &search,
							   wibo::ModuleInfo *importer, ForwarderLookup &lookup) {
	if (!info || !funcName) {
		return nullptr;
	}
	if (info->moduleStub && info->moduleStub->byName) {
		void *func = info->moduleStub->byName(funcName);
		if (func) {
			return func;
		}
	}
	ensureExportsInitialized(*info);
	auto it = info->exportNameToOrdinal.find(funcName);
	if (it != info->exportNameToOrdinal.end()) {
		return findExportByOrdinalInternal(info, it->second, search, importer, lookup);
	}
	return nullptr;
}

void *findExportByOrdinalInternal(wibo::ModuleInfo *info, uint16_t ordinal, const wibo::ModuleSearch &search,
								  wibo::ModuleInfo *importer, ForwarderLookup &lookup) {
	if (!info) {
		return nullptr;
	}
	if (info->moduleStub && info->moduleStub->nameByOrdinal) {
		const char *name = info->moduleStub->nameByOrdinal(ordinal);
		if (name && info->moduleStub->byName) {
			void *func = info->moduleStub->byName(name);
			if (func) {
				return func;
			}
		}
	}
	ensureExportsInitialized(*info);
	if (!info->exportsByOrdinal.empty() && ordinal >= info->exportOrdinalBase) {
		auto index = static_cast<size_t>(ordinal - info->exportOrdinalBase);
		if (index < info->exportsByOrdinal.size()) {
			auto forwarder = info->exportForwarders.find(index);
			if (forwarder != info->exportForwarders.end()) {
				const auto key = std::pair{info, ordinal};
				if (std::find(lookup.path.begin(), lookup.path.end(), key) != lookup.path.end()) {
					lookup.cycleDetected = true;
					return nullptr;
				}
				lookup.path.push_back(key);
				void *address = resolveForwardedExport(*info, forwarder->second.c_str(), search,
													   importer ? *importer : *info, lookup);
				lookup.path.pop_back();
				return address;
			}
			void *addr = info->exportsByOrdinal[index];
			if (addr) {
				return addr;
			}
		}
	}
	return nullptr;
}

bool ensureModuleReady(wibo::ModuleInfo &info, const wibo::ModuleSearch &search) {
	if (info.moduleStub && !info.moduleStub->dllData.empty() && !info.executable) {
		DEBUG_LOG("registerBuiltinModule: loading PE for %s\n", info.originalName.c_str());
		auto executable = std::make_unique<wibo::Executable>();
		if (!executable->loadPE(info.moduleStub->dllData, true)) {
			DEBUG_LOG("  loadPE failed for %s\n", info.originalName.c_str());
			return false;
		}
		info.executable = std::move(executable);
	}
	ensureExportsInitialized(info);
	if (!info.executable) {
		return true;
	}
	if (!info.executable->resolveImports(search, &info)) {
		return false;
	}
	if (!wibo::initializeModuleTls(info)) {
		return false;
	}
	if (!callDllMain(info, DLL_PROCESS_ATTACH, nullptr)) {
		kernel32::setLastError(ERROR_DLL_INIT_FAILED);
		return false;
	}
	return true;
}

} // namespace

namespace entry {

// Trampoline target for missing imports
void stubBase(SIZE_T index) {
	const char *func = stubFuncNames[index].empty() ? "<unknown>" : stubFuncNames[index].c_str();
	const char *dll = stubDlls[index].empty() ? "<unknown>" : stubDlls[index].c_str();
	fprintf(stderr, "wibo: call reached missing import %s from %s\n", func, dll);
	fflush(stderr);
#if defined(__APPLE__)
	// abort() raises SIGABRT through pthread_kill. Rosetta can wedge that call
	// indefinitely when a translated guest worker owns the fault, leaving every
	// sibling alive and the process unkillable. We are already in host context;
	// clear this thread's translated TEB state and let the kernel terminate the
	// process directly instead of delivering a synchronous signal.
	wibo::uninstallTebForCurrentThread();
	_exit(127);
#else
	abort();
#endif
}

} // namespace entry

namespace wibo {

void initializeModuleRegistry() { registry(); }

ModuleInfo *registerProcessModule(std::unique_ptr<Executable> executable, std::filesystem::path resolvedPath,
								  std::string originalName) {
	if (!executable) {
		return nullptr;
	}

	if (originalName.empty() && !resolvedPath.empty()) {
		originalName = resolvedPath.filename().string();
	}

	ParsedModuleName parsed = parseModuleName(originalName);
	std::string normalizedName = normalizedBaseKey(parsed);

	ModulePtr info = std::make_unique<ModuleInfo>();
	info->handle = toGuestPtr(executable->imageBase); // Use image base as handle for main module
	info->moduleStub = nullptr;
	info->originalName = std::move(originalName);
	info->normalizedName = std::move(normalizedName);
	info->resolvedPath = std::move(resolvedPath);
	info->executable = std::move(executable);
	info->refCount = UINT_MAX;

	ModuleInfo *raw = info.get();

	std::string storageKey;
	if (!raw->resolvedPath.empty()) {
		storageKey = storageKeyForPath(raw->resolvedPath);
	} else if (!raw->normalizedName.empty()) {
		storageKey = storageKeyForBuiltin(raw->normalizedName);
	}
	if (storageKey.empty()) {
		storageKey = normalizeAlias(raw->originalName);
	}

	auto reg = registry();
	reg->modulesByKey[storageKey] = std::move(info);

	if (!raw->resolvedPath.empty()) {
		registerExternalModuleAliases(*reg, raw->originalName, raw->resolvedPath, raw);
	} else {
		registerAlias(*reg, normalizeAlias(raw->originalName), raw);
		std::string baseAlias = normalizedBaseKey(parsed);
		if (baseAlias != raw->originalName) {
			registerAlias(*reg, baseAlias, raw);
		}
	}

	ensureExportsInitialized(*raw);

	auto pinAlias = [&](const std::string &alias) {
		if (!alias.empty()) {
			reg->pinnedAliases.insert(alias);
		}
	};
	reg->pinnedModules.insert(raw);
	pinAlias(storageKey);
	pinAlias(normalizeAlias(raw->originalName));
	pinAlias(raw->normalizedName);
	publishThreadNotificationSnapshot(*reg);

	return raw;
}

void shutdownModuleRegistry() {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	const bool collecting = g_collectingModules;
	g_collectingModules = true;
	std::vector<ModulePtr> targets;
	std::unordered_set<ModuleInfo *> notified;
	for (;;) {
		ModulePtr target;
		{
			auto reg = registry();
			for (auto &[key, module] : reg->modulesByKey) {
				if (!module->moduleStub && !notified.contains(module.get()) &&
					(!target || module->initializationOrder > target->initializationOrder))
					target = module;
			}
		}
		if (!target)
			break;
		notified.insert(target.get());
		targets.push_back(target);
		if (!target->detachNotificationsDelivered) {
			target->detachNotificationsDelivered = true;
			if (target->tlsInfo.hasTls)
				runModuleTlsCallbacks(*target, TLS_PROCESS_DETACH);
			callDllMain(*target, DLL_PROCESS_DETACH, reinterpret_cast<LPVOID>(1));
		}
	}
	targets.insert(targets.end(), g_failedModuleLeases.begin(), g_failedModuleLeases.end());
	g_failedModuleLeases.clear();
	for (const ModulePtr &target : targets)
		releaseModuleTls(*target);
	{
		auto reg = registry();
		reg->modulesByKey.clear();
		reg->modulesByAlias.clear();
		reg->dllDirectory.reset();
		reg->dllDirectorySpecifiedName.clear();
		reg->initialized = false;
		g_threadNotificationSnapshot.reset();
	}
	g_modules.clear();
	g_collectingModules = collecting;
}

ModuleInfo *moduleInfoFromHandle(HMODULE module) {
	if (isMainModule(module)) {
		return wibo::mainModule;
	}
	if (!module) {
		return nullptr;
	}
	auto reg = registry();
	for (auto &pair : reg->modulesByKey) {
		wibo::ModuleInfo *info = pair.second.get();
		if (!info) {
			continue;
		}
		if (info->handle == module) {
			return info;
		}
		if (info->executable && info->executable->imageBase == reinterpret_cast<void *>(module)) {
			return info;
		}
	}
	return nullptr;
}

void setDllDirectoryOverride(const std::filesystem::path &path, std::u16string specifiedName) {
	auto canonical = path.empty() ? path : files::canonicalPath(path);
	auto reg = registry();
	reg->dllDirectory = canonical;
	reg->dllDirectorySpecifiedName = std::move(specifiedName);
}

void clearDllDirectoryOverride() {
	auto reg = registry();
	reg->dllDirectory.reset();
	reg->dllDirectorySpecifiedName.clear();
}

std::optional<std::filesystem::path> dllDirectoryOverride() {
	auto reg = registry();
	return reg->dllDirectory;
}

std::u16string dllDirectoryName() {
	auto reg = registry();
	return reg->dllDirectorySpecifiedName;
}

ModuleInfo *moduleInfoFromAddress(void *addr) {
	if (!addr) {
		return nullptr;
	}
	auto reg = registry();
	return moduleFromAddress(*reg, addr);
}

void *loadedImageBaseFromAddress(void *addr) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	auto reg = registry();
	ModuleInfo *info = moduleFromAddress(*reg, addr);
	return info && info->executable ? info->executable->imageBase : nullptr;
}

bool initializeModuleTls(ModuleInfo &module) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (module.tlsInfo.hasTls) {
		return true;
	}
	if (!module.executable) {
		return true;
	}
	Executable &exec = *module.executable;
	if (exec.tlsDirectoryRVA == 0 || exec.tlsDirectorySize < kMinTlsDirectorySize) {
		return true;
	}
	auto *tlsDirectoryRaw = exec.fromRVA<uint8_t>(exec.tlsDirectoryRVA);
	if (!tlsDirectoryRaw) {
		return false;
	}
	ImageTlsDirectory tlsDirectory{};
	// Older images can provide a shorter directory; absent trailing fields remain zero-initialized.
	size_t copySize = std::min<size_t>(exec.tlsDirectorySize, sizeof(tlsDirectory));
	std::memcpy(&tlsDirectory, tlsDirectoryRaw, copySize);

	auto &info = module.tlsInfo;
	info.templateSize = (tlsDirectory.EndAddressOfRawData > tlsDirectory.StartAddressOfRawData)
							? tlsDirectory.EndAddressOfRawData - tlsDirectory.StartAddressOfRawData
							: 0;
	info.zeroFillSize = tlsDirectory.SizeOfZeroFill;
	info.characteristics = tlsDirectory.Characteristics;
	info.templateData = reinterpret_cast<uint8_t *>(resolveModuleAddress(exec, tlsDirectory.StartAddressOfRawData));
	info.indexLocation = reinterpret_cast<DWORD *>(resolveModuleAddress(exec, tlsDirectory.AddressOfIndex));
	info.callbacks.clear();
	uintptr_t callbacksArray = resolveModuleAddress(exec, tlsDirectory.AddressOfCallBacks);
	if (callbacksArray) {
		auto callbackPtr = reinterpret_cast<GUEST_PTR *>(callbacksArray);
		while (callbackPtr && *callbackPtr) {
			info.callbacks.push_back(reinterpret_cast<PIMAGE_TLS_CALLBACK>(resolveModuleAddress(exec, *callbackPtr)));
			++callbackPtr;
		}
	}
	info.allocationSize = info.templateSize + info.zeroFillSize;
	info.threadAllocations.clear();

	DWORD loaderIndex = tls::kInvalidTlsIndex;
	size_t requiredModuleCapacity = 0;
	{
		auto reg = registry();
		loaderIndex = allocateModuleTlsSlot(*reg, module);
		requiredModuleCapacity = reg->tlsModuleSlots.size();
	}
	if (!wibo::tls::ensureModulePointerCapacity(requiredModuleCapacity)) {
		auto reg = registry();
		releaseModuleTlsSlot(*reg, loaderIndex);
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	info.loaderIndex = loaderIndex;
	if (info.indexLocation) {
		*info.indexLocation = loaderIndex;
	}

	DWORD apiIndex = tls::reserveSlot();
	if (apiIndex == tls::kInvalidTlsIndex) {
		auto reg = registry();
		releaseModuleTlsSlot(*reg, loaderIndex);
		info.loaderIndex = tls::kInvalidTlsIndex;
		if (info.indexLocation) {
			*info.indexLocation = 0;
		}
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	info.index = apiIndex;
	info.hasTls = true;

	struct AllocContext {
		ModuleInfo *module;
		bool success;
	};
	AllocContext ctx{&module, true};
	wibo::tls::forEachTib(
		[](TEB *tib, void *opaque) {
			auto *context = static_cast<AllocContext *>(opaque);
			if (!context->success) {
				return;
			}
			if (!allocateModuleTlsForThread(*context->module, tib)) {
				context->success = false;
			}
		},
		&ctx);
	if (!ctx.success) {
		for (auto &[thread, block] : info.threadAllocations) {
			if (block) {
				wibo::heap::guestFree(fromGuestPtr(block));
			}
			if (info.loaderIndex != tls::kInvalidTlsIndex) {
				wibo::tls::clearModulePointer(thread, info.loaderIndex);
			}
			if (wibo::tls::getValue(thread, info.index) == block) {
				wibo::tls::setValue(thread, info.index, GUEST_NULL);
			}
		}
		info.threadAllocations.clear();
		wibo::tls::releaseSlot(info.index);
		info.index = tls::kInvalidTlsIndex;
		{
			auto reg = registry();
			releaseModuleTlsSlot(*reg, loaderIndex);
		}
		info.loaderIndex = tls::kInvalidTlsIndex;
		info.hasTls = false;
		if (info.indexLocation) {
			*info.indexLocation = 0;
		}
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	runModuleTlsCallbacks(module, TLS_PROCESS_ATTACH);
	kernel32::setLastError(ERROR_SUCCESS);
	return true;
}

void releaseModuleTls(ModuleInfo &module) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (!module.tlsInfo.hasTls) {
		return;
	}
	auto &info = module.tlsInfo;
	for (auto &[tib, block] : info.threadAllocations) {
		if (tib) {
			if (wibo::tls::getValue(tib, info.index) == block) {
				wibo::tls::setValue(tib, info.index, GUEST_NULL);
			}
			if (info.loaderIndex != tls::kInvalidTlsIndex) {
				wibo::tls::clearModulePointer(tib, info.loaderIndex);
			}
		}
		if (block) {
			wibo::heap::guestFree(fromGuestPtr(block));
		}
	}
	info.threadAllocations.clear();
	if (info.index != tls::kInvalidTlsIndex) {
		wibo::tls::releaseSlot(info.index);
	}
	{
		auto reg = registry();
		if (info.loaderIndex != tls::kInvalidTlsIndex) {
			releaseModuleTlsSlot(*reg, info.loaderIndex);
		}
	}
	info = wibo::ModuleTlsInfo{};
}

void notifyDllThreadAttach() {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	// The process image has no DllMain, but its PE static TLS and TLS callbacks
	// participate in every thread attach just like a DLL. Keep it separate from
	// the DLL notification snapshot so callDllMain ordering remains explicit.
	if (wibo::mainModule && wibo::mainModule->tlsInfo.hasTls) {
		if (!allocateModuleTlsForThread(*wibo::mainModule, currentThreadTeb)) {
			DEBUG_LOG("notifyDllThreadAttach: failed to allocate TLS for process image %s\n",
					  wibo::mainModule->originalName.c_str());
		}
		runModuleTlsCallbacks(*wibo::mainModule, TLS_THREAD_ATTACH);
	}
	auto targets = snapshotThreadNotificationModules();
	if (!targets) {
		kernel32::setLastError(ERROR_SUCCESS);
		return;
	}
	for (const ModulePtr &target : *targets) {
		ModuleInfo *info = target.get();
		if (info && info->tlsInfo.hasTls) {
			if (!allocateModuleTlsForThread(*info, currentThreadTeb)) {
				DEBUG_LOG("notifyDllThreadAttach: failed to allocate TLS for %s\n", info->originalName.c_str());
			}
			runModuleTlsCallbacks(*info, TLS_THREAD_ATTACH);
		}
	}
	for (const ModulePtr &target : *targets) {
		ModuleInfo *info = target.get();
		callDllMain(*info, DLL_THREAD_ATTACH, nullptr);
	}
	kernel32::setLastError(ERROR_SUCCESS);
}

void notifyDllThreadDetach() {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	auto targets = snapshotThreadNotificationModules();
	if (targets) {
		for (auto it = targets->rbegin(); it != targets->rend(); ++it) {
			ModuleInfo *info = it->get();
			if (info && info->tlsInfo.hasTls) {
				runModuleTlsCallbacks(*info, TLS_THREAD_DETACH);
			}
		}
		for (auto it = targets->rbegin(); it != targets->rend(); ++it) {
			callDllMain(**it, DLL_THREAD_DETACH, nullptr);
		}
		for (auto it = targets->rbegin(); it != targets->rend(); ++it) {
			ModuleInfo *info = it->get();
			if (info && info->tlsInfo.hasTls) {
				freeModuleTlsForThread(*info, currentThreadTeb);
			}
		}
	}
	if (wibo::mainModule && wibo::mainModule->tlsInfo.hasTls) {
		runModuleTlsCallbacks(*wibo::mainModule, TLS_THREAD_DETACH);
		freeModuleTlsForThread(*wibo::mainModule, currentThreadTeb);
	}
	kernel32::setLastError(ERROR_SUCCESS);
}

BOOL disableThreadNotifications(HMODULE module) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (!module)
		return FALSE;
	ModuleInfo *info = moduleInfoFromHandle(module);
	if (!info) {
		return FALSE;
	}
	if (info->tlsInfo.hasTls) {
		DEBUG_LOG("disableThreadNotifications: %s uses static TLS\n", info->originalName.c_str());
		return FALSE;
	}
	info->threadNotificationsEnabled = false;
	{
		auto reg = registry();
		publishThreadNotificationSnapshot(*reg);
	}
	return TRUE;
}

ModuleInfo *findLoadedModule(const char *name) {
	if (!name || *name == '\0') {
		return wibo::mainModule;
	}
	auto reg = registry();
	ParsedModuleName parsed = parseModuleName(name);
	std::string alias = normalizedBaseKey(parsed);
	ModuleInfo *info = findByAlias(*reg, alias);
	if (!info) {
		info = findByAlias(*reg, normalizeAlias(name));
	}
	return info;
}

HMODULE findLoadedModuleHandle(const char *name) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (!name || !name[0])
		return NO_HANDLE;
	if (!std::strpbrk(name, "\\/:")) {
		ModuleInfo *info = findLoadedModule(name);
		return info ? info->handle : NO_HANDLE;
	}

	const auto parsed = parseModuleName(name);
	auto reg = registry();
	for (const auto &base : candidateModuleNames(parsed)) {
		const auto qualified = parsed.directory.empty() ? base : parsed.directory + "\\" + base;
		const auto mapped = files::pathFromWindows(qualified.c_str());
		if (!mapped.is_absolute())
			continue;
		std::error_code ec;
		auto path = std::filesystem::weakly_canonical(mapped, ec);
		if (ec)
			continue;
		const auto key = normalizeAlias(files::pathToWindows(path));
		const auto found = reg->modulesByKey.find(key);
		if (found != reg->modulesByKey.end() && !found->second->resolvedPath.empty())
			return found->second->handle;
	}
	return NO_HANDLE;
}

HMODULE acquireModuleHandle(const char *name, bool fromAddress, bool pin, bool unchanged) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	ModuleInfo *info = fromAddress ? moduleInfoFromAddress(const_cast<char *>(name)) : findLoadedModule(name);
	if (!info)
		return NO_HANDLE;
	if (pin)
		info->refCount = UINT_MAX;
	else if (!unchanged && info->refCount != UINT_MAX)
		++info->refCount;
	return info->handle;
}

static ModuleInfo *loadModuleInternal(const std::string &dllName, const ModuleSearch &search, bool explicitReference) {
	auto reg = registry();
	ParsedModuleName parsed = parseModuleName(dllName);
	DWORD diskError = ERROR_SUCCESS;
	auto reuseExternal = [&](ModuleInfo *info) -> ModuleInfo * {
		if (explicitReference && info->refCount != UINT_MAX)
			++info->refCount;
		if (info->detachNotificationsDelivered && !info->loadInProgress) {
			info->loadInProgress = true;
			info->detachNotificationsDelivered = false;
			info->processAttachCalled = false;
			reg.lock.unlock();
			if (info->tlsInfo.hasTls)
				runModuleTlsCallbacks(*info, DLL_PROCESS_ATTACH);
			BOOL attached = callDllMain(*info, DLL_PROCESS_ATTACH, nullptr);
			reg.lock.lock();
			info->loadInProgress = false;
			if (!attached) {
				if (explicitReference && info->refCount != UINT_MAX)
					--info->refCount;
				info->detachNotificationsDelivered = true;
				diskError = ERROR_DLL_INIT_FAILED;
				kernel32::setLastError(diskError);
				return nullptr;
			}
			info->initializationOrder = ++g_initializationOrder;
			publishThreadNotificationSnapshot(*reg);
		}
		return info;
	};

	auto tryLoadExternal = [&](const std::filesystem::path &path) -> ModuleInfo * {
		std::string key = storageKeyForPath(path);
		auto existingIt = reg->modulesByKey.find(key);
		if (existingIt != reg->modulesByKey.end()) {
			ModuleInfo *info = existingIt->second.get();
			registerExternalModuleAliases(*reg, dllName, files::canonicalPath(path), info);
			return reuseExternal(info);
		}
		reg.lock.unlock();

		DEBUG_LOG("  loading external module from %s\n", path.c_str());
		FILE *file = fopen(path.c_str(), "rb");
		if (!file) {
			perror("loadModule");
			reg.lock.lock();
			diskError = ERROR_MOD_NOT_FOUND;
			return nullptr;
		}

		auto executable = std::make_unique<Executable>();
		if (!executable->loadPE(file, true)) {
			DEBUG_LOG("  loadPE failed for %s\n", path.c_str());
			fclose(file);
			reg.lock.lock();
			diskError = ERROR_BAD_EXE_FORMAT;
			return nullptr;
		}
		fclose(file);

		ModulePtr info = std::make_unique<ModuleInfo>();
		HMODULE handle = static_cast<HMODULE>(toGuestPtr(executable->imageBase));
		g_modules[handle] = info;
		info->handle = handle;
		info->moduleStub = nullptr;
		info->originalName = dllName;
		info->normalizedName = normalizedBaseKey(parsed);
		info->resolvedPath = files::canonicalPath(path);
		info->executable = std::move(executable);
		info->refCount = explicitReference ? 1 : 0;
		info->loadInProgress = true;

		reg.lock.lock();
		ModuleInfo *raw = info.get();
		reg->modulesByKey[key] = std::move(info);
		registerExternalModuleAliases(*reg, dllName, raw->resolvedPath, raw);
		auto discardModule = [&] {
			auto discarded = unregisterModule(*reg, raw);
			g_failedModuleLeases.push_back(std::move(discarded));
		};
		if (raw->executable->isDll) {
			reg.lock.unlock();
			ensureExportsInitialized(*raw);
			if (!raw->executable->resolveImports(search, raw)) {
				DEBUG_LOG("  resolveImports failed for %s\n", raw->originalName.c_str());
				reg.lock.lock();
				diskError = kernel32::getLastError();
				discardModule();
				return nullptr;
			}
			if (!initializeModuleTls(*raw)) {
				DEBUG_LOG("  initializeModuleTls failed for %s\n", raw->originalName.c_str());
				reg.lock.lock();
				diskError = kernel32::getLastError();
				discardModule();
				return nullptr;
			}
			BOOL attached = callDllMain(*raw, DLL_PROCESS_ATTACH, nullptr);
			reg.lock.lock();
			if (!attached) {
				DEBUG_LOG("  DllMain failed for %s\n", raw->originalName.c_str());
				discardModule();
				diskError = ERROR_DLL_INIT_FAILED;
				kernel32::setLastError(ERROR_DLL_INIT_FAILED);
				return nullptr;
			}
			publishThreadNotificationSnapshot(*reg);
		}
		raw->loadInProgress = false;
		raw->initializationOrder = ++g_initializationOrder;
		return raw;
	};

	auto resolveAndLoadExternal = [&]() -> ModuleInfo * {
		auto resolvedPath = resolveModuleOnDisk(*reg, dllName, search);
		if (!resolvedPath) {
			DEBUG_LOG("  module not found on disk\n");
			diskError = ERROR_MOD_NOT_FOUND;
			return nullptr;
		}
		return tryLoadExternal(*resolvedPath);
	};

	std::string alias = normalizedBaseKey(parsed);
	ModuleInfo *existing = findByAlias(*reg, alias);
	if (!existing) {
		existing = findByAlias(*reg, normalizeAlias(dllName));
	}
	if (existing) {
		DEBUG_LOG("  found existing module alias %s (builtin=%d)\n", alias.c_str(), existing->moduleStub != nullptr);
		if (existing->moduleStub == nullptr) {
			DEBUG_LOG("  returning existing external module %s\n", existing->originalName.c_str());
			return reuseExternal(existing);
		}
		bool pinned = reg->pinnedModules.contains(existing);
		if (!pinned && search.flags != ModuleSearch::SystemDirectory &&
			(search.flags == 0 || !parsed.directory.empty())) {
			if (ModuleInfo *external = resolveAndLoadExternal()) {
				DEBUG_LOG("  replaced builtin module %s with external copy\n", dllName.c_str());
				return external;
			} else if (diskError != ERROR_MOD_NOT_FOUND) {
				kernel32::setLastError(diskError);
				return nullptr;
			}
		}
		DEBUG_LOG("  returning builtin module %s\n", existing->originalName.c_str());
		ModuleInfo *builtin = existing;
		reg.lock.unlock();
		if (!ensureModuleReady(*builtin, search)) {
			return nullptr;
		}
		return builtin;
	}

	if (ModuleInfo *external = resolveAndLoadExternal()) {
		DEBUG_LOG("  loaded external module %s\n", dllName.c_str());
		return external;
	} else if (diskError != ERROR_MOD_NOT_FOUND) {
		kernel32::setLastError(diskError);
		return nullptr;
	}

	auto fallbackAlias = normalizedBaseKey(parsed);
	ModuleInfo *builtin = nullptr;
	auto builtinIt = reg->builtinAliasMap.find(fallbackAlias);
	if (builtinIt != reg->builtinAliasMap.end()) {
		builtin = builtinIt->second;
	}
	if (!builtin) {
		builtinIt = reg->builtinAliasMap.find(normalizeAlias(dllName));
		if (builtinIt != reg->builtinAliasMap.end()) {
			builtin = builtinIt->second;
		}
	}
	if (builtin && builtin->moduleStub != nullptr) {
		DEBUG_LOG("  falling back to builtin module %s\n", builtin->originalName.c_str());
		reg.lock.unlock();
		if (!ensureModuleReady(*builtin, search)) {
			return nullptr;
		}
		return builtin;
	}

	kernel32::setLastError((diskError != ERROR_SUCCESS) ? diskError : ERROR_MOD_NOT_FOUND);
	return nullptr;
}

static std::optional<std::string> providerApiSetHost(const std::string &contract) {
	std::vector<uint8_t> response;
	if (!provider::request({"api-set-host", contract}, response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return std::nullopt;
	}
	provider::Reader reader(response);
	int32_t status = 0;
	constexpr DWORD kInvalidData = 13;
	if (!reader.header(status)) {
		kernel32::setLastError(kInvalidData);
		return std::nullopt;
	}
	if (status) {
		kernel32::setLastError(reader.done() ? static_cast<DWORD>(status) : kInvalidData);
		return std::nullopt;
	}
	std::u16string wideHost;
	std::string host;
	if (!reader.text(wideHost) || wideHost.empty() || wideHost.size() > 260 || !reader.done() ||
		wideHost.find_first_of(u"/\\:") != std::u16string::npos || wideHost.find(u'\0') != std::u16string::npos ||
		!provider::encodeUtf8(wideHost, host)) {
		kernel32::setLastError(kInvalidData);
		return std::nullopt;
	}
	host = normalizeAlias(host);
	if (!host.ends_with(".dll") || host.starts_with("api-") || host.starts_with("ext-")) {
		kernel32::setLastError(kInvalidData);
		return std::nullopt;
	}
	return host;
}

static ModuleInfo *loadModuleWithReference(const char *dllName, const ModuleSearch &search, bool explicitReference) {
	if (!dllName || *dllName == '\0') {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	DEBUG_LOG("loadModule(%s)\n", dllName);
	std::string_view requested{dllName};

	const auto parsed = parseModuleName(requested);
	std::string normalized = normalizedBaseKey(parsed);

	for (auto &[alias, module] : kApiSet) {
		if (alias == normalized) {
			DEBUG_LOG("  resolved api set %s -> %s\n", alias.data(), module.data());
			requested = module;
			normalized = module;
			break;
		}
	}

	if (parsed.directory.empty() && (normalized.starts_with("api-") || normalized.starts_with("ext-")) &&
		provider::configured()) {
		if (findLoadedModule(normalized.c_str()))
			return loadModuleInternal(normalized, search, explicitReference);
		auto host = providerApiSetHost(normalized);
		if (!host)
			return nullptr;
		DEBUG_LOG("  resolved api set %s -> %s\n", normalized.c_str(), host->c_str());
		auto *info = loadModuleInternal(*host, search, explicitReference);
		if (info) {
			auto reg = registry();
			registerAlias(*reg, normalized, info);
			registerAlias(*reg, normalizeAlias(parsed.original), info);
		}
		return info;
	}

	// DWORD lastError = kernel32::getLastError();
	if (auto *info = loadModuleInternal(std::string{requested}, search, explicitReference)) {
		return info;
	}
	if (kernel32::getLastError() != ERROR_MOD_NOT_FOUND) {
		return nullptr;
	}

	// kernel32::setLastError(lastError);
	// for (auto &[module, fallback] : kFallbacks) {
	// 	if (module == normalized) {
	// 		DEBUG_LOG("  trying fallback %s -> %s\n", module.data(), fallback.data());
	// 		return loadModuleInternal(std::string{fallback});
	// 	}
	// }

	// kernel32::setLastError(ERROR_MOD_NOT_FOUND);
	return nullptr;
}

ModuleInfo *loadModule(const char *dllName, DWORD flags) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (flags & ~(ModuleSearch::AlteredPath | ModuleSearch::DirectoryMask)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return nullptr;
	}
	if ((flags & ModuleSearch::AlteredPath) && (flags & ModuleSearch::DirectoryMask)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	std::filesystem::path topDirectory;
	if (flags & (ModuleSearch::AlteredPath | ModuleSearch::DllDirectory)) {
		std::string name = dllName ? dllName : "";
		std::replace(name.begin(), name.end(), '/', '\\');
		const bool absolute =
			!name.empty() && (name.front() == '\\' || (name.size() > 2 && name[1] == ':' && name[2] == '\\'));
		if (!absolute && ((flags & ModuleSearch::DllDirectory) || name.find('\\') != std::string::npos)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return nullptr;
		}
		if (absolute)
			topDirectory = files::pathFromWindows(name.c_str()).parent_path();
	}
	ModuleSearch search;
	search.flags = flags;
	search.directoriesResolved = true;
	{
		auto reg = registry();
		search.directories = collectSearchDirectories(*reg, flags, topDirectory);
	}
	return loadModule(dllName, search);
}

ModuleInfo *loadModule(const char *dllName, const ModuleSearch &search) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	auto *info = loadModuleWithReference(dllName, search, true);
	if (!info) {
		DWORD error = kernel32::getLastError();
		collectUnusedModules();
		kernel32::setLastError(error);
	}
	return info;
}

ModuleInfo *loadDependency(ModuleInfo &importer, const char *dllName, const ModuleSearch &search) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	auto *info = loadModuleWithReference(dllName, search, false);
	if (info) {
		auto reg = registry();
		if (std::find(importer.dependencies.begin(), importer.dependencies.end(), info) == importer.dependencies.end())
			importer.dependencies.push_back(info);
	} else {
		DWORD error = kernel32::getLastError();
		collectUnusedModules();
		kernel32::setLastError(error);
	}
	return info;
}

void freeModule(ModuleInfo *info) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	if (!info || info->refCount == UINT_MAX || info->refCount == 0)
		return;
	--info->refCount;
	collectUnusedModules();
}

void *findExportByName(ModuleInfo *info, const char *funcName, const ModuleSearch &search, ModuleInfo *importer) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	ForwarderLookup lookup;
	lookup.staticImport = importer != nullptr;
	return findExportByNameInternal(info, funcName, search, importer, lookup);
}

void *findExportByOrdinal(ModuleInfo *info, uint16_t ordinal, const ModuleSearch &search, ModuleInfo *importer) {
	std::lock_guard loaderLock(g_loaderNotificationMutex);
	ForwarderLookup lookup;
	lookup.staticImport = importer != nullptr;
	return findExportByOrdinalInternal(info, ordinal, search, importer, lookup);
}

void *resolveFuncByName(ModuleInfo *info, const char *funcName, const ModuleSearch &search, ModuleInfo *importer) {
	void *func = findExportByName(info, funcName, search, importer);
	if (func) {
		return func;
	}
	if (info && info->moduleStub) {
		const char *safeFunc = funcName ? funcName : "";
		return reinterpret_cast<void *>(resolveMissingFuncName(info->originalName.c_str(), safeFunc));
	}
	return nullptr;
}

void *resolveFuncByOrdinal(ModuleInfo *info, uint16_t ordinal, const ModuleSearch &search, ModuleInfo *importer) {
	void *func = findExportByOrdinal(info, ordinal, search, importer);
	if (func) {
		return func;
	}
	if (info && info->moduleStub) {
		return reinterpret_cast<void *>(resolveMissingFuncOrdinal(info->originalName.c_str(), ordinal));
	}
	return nullptr;
}

void *resolveMissingImportByName(const char *dllName, const char *funcName) {
	const char *safeDll = dllName ? dllName : "";
	const char *safeFunc = funcName ? funcName : "";
	[[maybe_unused]] auto reg = registry();
	return reinterpret_cast<void *>(resolveMissingFuncName(safeDll, safeFunc));
}

void *resolveMissingImportByOrdinal(const char *dllName, uint16_t ordinal) {
	const char *safeDll = dllName ? dllName : "";
	[[maybe_unused]] auto reg = registry();
	return reinterpret_cast<void *>(resolveMissingFuncOrdinal(safeDll, ordinal));
}

Executable *executableFromModule(HMODULE module) {
	ModuleInfo *info = moduleInfoFromHandle(module);
	if (!info) {
		return nullptr;
	}
	if (!info->executable && !info->resolvedPath.empty()) {
		FILE *file = fopen(info->resolvedPath.c_str(), "rb");
		if (!file) {
			perror("executableFromModule");
			return nullptr;
		}
		auto executable = std::make_unique<Executable>();
		if (!executable->loadPE(file, false)) {
			DEBUG_LOG("executableFromModule: failed to load %s\n", info->resolvedPath.c_str());
			fclose(file);
			return nullptr;
		}
		fclose(file);
		info->executable = std::move(executable);
	}
	return info->executable.get();
}

std::unordered_map<std::string, ModulePtr> allLoadedModules() {
	auto reg = registry();
	return reg->modulesByKey;
}

} // namespace wibo
