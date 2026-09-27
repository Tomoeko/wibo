#include "winreg.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "kernel32/processenv.h"
#include "kernel32/sysinfoapi.h"
#include "strutil.h"
#include "system_provider.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct RegistryKeyObject : ObjectBase {
	static constexpr ObjectType kType = ObjectType::RegistryKey;

	std::u16string canonicalPath;
	bool predefined = false;
#ifdef WIBO_GUEST_64
	std::string providerView = "64";
#else
	std::string providerView = "32";
#endif

	RegistryKeyObject() : ObjectBase(kType) {}
	explicit RegistryKeyObject(std::u16string path) : ObjectBase(kType), canonicalPath(std::move(path)) {}
};

struct PredefinedKeyInfo {
	HKEY value;
	const char16_t *name;
};

constexpr PredefinedKeyInfo kPredefinedKeyInfos[] = {
	{HKEY_CLASSES_ROOT, u"HKEY_CLASSES_ROOT"},		   {HKEY_CURRENT_USER, u"HKEY_CURRENT_USER"},
	{HKEY_LOCAL_MACHINE, u"HKEY_LOCAL_MACHINE"},	   {HKEY_USERS, u"HKEY_USERS"},
	{HKEY_PERFORMANCE_DATA, u"HKEY_PERFORMANCE_DATA"}, {HKEY_CURRENT_CONFIG, u"HKEY_CURRENT_CONFIG"},
};

constexpr size_t kPredefinedKeyCount = std::size(kPredefinedKeyInfos);
constexpr REGSAM kLegacyOpenAccess = 0x02000000; // MAXIMUM_ALLOWED
constexpr REGSAM kKeyEnumerateSubkeys = 0x00000008;
constexpr REGSAM kKeyRead = 0x00020019;
constexpr REGSAM kKeyWrite = 0x00020006;
constexpr REGSAM kKnownKeyAccess = 0x000f003f;
constexpr REGSAM kRepresentedKeyAccess = 0x0002000f;

std::mutex g_registryMutex;
std::unordered_set<std::u16string> g_existingKeys;

struct RegistryValue {
	DWORD type = 0;
	std::vector<BYTE> data{};
	std::u16string name;
};

// Values belong to a key path, not an open handle. The registry is process-local;
// this does not add persistence or ACL checks.
using RegistryValues = std::unordered_map<std::u16string, RegistryValue>;
std::unordered_map<std::u16string, RegistryValues> g_registryValues;
constexpr DWORD kRegSz = 1;
constexpr DWORD kRegExpandSz = 2;
constexpr DWORD kRegMultiSz = 7;
constexpr LSTATUS kErrorMoreData = 234;

// Provider data is a read-only process snapshot. Guest writes remain in the
// existing local store and never modify the provider's environment.
std::unordered_map<std::u16string, LSTATUS> g_providerKeys;
std::unordered_map<std::u16string, RegistryValues> g_providerSnapshots;
struct RegistrySubkey {
	std::u16string name;
	std::u16string className;
	FILETIME lastWriteTime{};
};
struct LocalRegistryKey {
	std::u16string canonicalPath;
	RegistrySubkey information;
};
std::unordered_map<std::u16string, LocalRegistryKey> g_localKeys;
std::unordered_map<std::u16string, FILETIME> g_localWriteTimes;
std::unordered_map<std::u16string, std::vector<RegistrySubkey>> g_providerSubkeys;
std::unordered_map<std::u16string, std::string> g_registryAnsiCache;

bool pathWithin(std::u16string_view path, std::u16string_view parent) {
	return path == parent || (path.starts_with(parent) && path.size() > parent.size() && path[parent.size()] == u'\\');
}

std::u16string localCacheKey(const std::u16string &path, const std::string &view) {
	// Ordinary per-user keys and system keys are shared across the two views.
	// Merged class namespaces and registry reflection are not represented here.
	const bool shared =
		(pathWithin(path, u"hkey_current_user") && !pathWithin(path, u"hkey_current_user\\software\\classes")) ||
		pathWithin(path, u"hkey_local_machine\\system");
	return path + (shared ? u"|shared" : (view == "64" ? u"|64" : u"|32"));
}

LSTATUS registryAccess(REGSAM requested, REGSAM &access) {
	if (requested & GENERIC_READ)
		requested = (requested & ~GENERIC_READ) | kKeyRead;
	if (requested & GENERIC_WRITE)
		requested = (requested & ~GENERIC_WRITE) | kKeyWrite;
	if (requested & GENERIC_EXECUTE)
		requested = (requested & ~GENERIC_EXECUTE) | kKeyRead;
	if (requested & GENERIC_ALL)
		requested = (requested & ~GENERIC_ALL) | kKnownKeyAccess;
	if (requested & ~(kKnownKeyAccess | kLegacyOpenAccess))
		return ERROR_NOT_SUPPORTED;
	// MAXIMUM_ALLOWED retains only represented operations, without claiming a DACL evaluation.
	access = requested & kLegacyOpenAccess ? kRepresentedKeyAccess : requested;
	return ERROR_SUCCESS;
}

LSTATUS registryView(REGSAM access, const std::string &inherited, std::string &view) {
	if ((access & (KEY_WOW64_64KEY | KEY_WOW64_32KEY)) == (KEY_WOW64_64KEY | KEY_WOW64_32KEY))
		return ERROR_INVALID_PARAMETER;
	view = access & KEY_WOW64_64KEY ? "64" : (access & KEY_WOW64_32KEY ? "32" : inherited);
	return ERROR_SUCCESS;
}

std::u16string canonicalizeValueName(LPCWSTR name);
struct ProviderValue {
	LSTATUS status = ERROR_FILE_NOT_FOUND;
	RegistryValue value;
};
std::unordered_map<std::u16string, ProviderValue> g_providerValues;

std::u16string providerCacheKey(const std::u16string &path, const std::string &view) {
	return path + (view == "64" ? u"|64" : u"|32");
}

LSTATUS providerOpen(const std::u16string &path, const std::string &view) {
	if (!wibo::provider::configured())
		return ERROR_FILE_NOT_FOUND;
	const auto cacheKey = providerCacheKey(path, view);
	if (auto found = g_providerKeys.find(cacheKey); found != g_providerKeys.end())
		return found->second;
	std::string encoded;
	if (!wibo::provider::encodeUtf8(path, encoded))
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> response;
	const bool snapshot = wibo::provider::request({"registry-snapshot", encoded, view}, response);
	if (!snapshot && !wibo::provider::request({"registry-open", encoded, view}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	RegistryValues values;
	if (status == ERROR_SUCCESS && snapshot) {
		uint32_t count = 0;
		if (!reader.number(count) || count > 4096)
			return ERROR_INVALID_DATA;
		for (uint32_t i = 0; i < count; ++i) {
			std::u16string name;
			RegistryValue value;
			if (!reader.text(name) || name.find(u'\0') != std::u16string::npos || !reader.number(value.type) ||
				!reader.bytes(value.data))
				return ERROR_INVALID_DATA;
			const auto canonicalName = canonicalizeValueName(reinterpret_cast<LPCWSTR>(name.c_str()));
			value.name = std::move(name);
			values.insert_or_assign(canonicalName, std::move(value));
		}
	}
	if (!reader.done())
		return ERROR_INVALID_DATA;
	if (snapshot && status == ERROR_SUCCESS)
		g_providerSnapshots.emplace(cacheKey, std::move(values));
	g_providerKeys.emplace(cacheKey, status);
	return status;
}

LSTATUS providerQuery(const RegistryKeyObject &key, const std::u16string &name, RegistryValue &value) {
	if (!wibo::provider::configured())
		return ERROR_FILE_NOT_FOUND;
	const LSTATUS opened = providerOpen(key.canonicalPath, key.providerView);
	if (opened != ERROR_SUCCESS)
		return opened;
	auto cacheKey = providerCacheKey(key.canonicalPath, key.providerView);
	if (auto snapshot = g_providerSnapshots.find(cacheKey); snapshot != g_providerSnapshots.end()) {
		auto entry = snapshot->second.find(name);
		if (entry == snapshot->second.end())
			return ERROR_FILE_NOT_FOUND;
		value = entry->second;
		return ERROR_SUCCESS;
	}
	cacheKey.push_back(0);
	cacheKey += name;
	if (auto found = g_providerValues.find(cacheKey); found != g_providerValues.end()) {
		value = found->second.value;
		return found->second.status;
	}
	std::string pathText, nameText;
	if (!wibo::provider::encodeUtf8(key.canonicalPath, pathText) || !wibo::provider::encodeUtf8(name, nameText))
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"registry-query", pathText, nameText, key.providerView}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	if (status == ERROR_SUCCESS) {
		if (!reader.number(value.type) || !reader.bytes(value.data))
			return ERROR_INVALID_DATA;
	}
	if (!reader.done())
		return ERROR_INVALID_DATA;
	g_providerValues.emplace(std::move(cacheKey), ProviderValue{status, value});
	return status;
}

bool isRegistryString(DWORD type) { return type == kRegSz || type == kRegExpandSz || type == kRegMultiSz; }

std::u16string canonicalizeValueName(LPCWSTR name) {
	std::u16string result;
	if (name) {
		for (; *name; ++name) {
			result.push_back(static_cast<char16_t>(wcharToLower(*name)));
		}
	}
	// Unlike key paths, slash characters are literal parts of a value name.
	return result;
}

std::u16string canonicalizeKeySegment(const std::u16string &input) {
	std::u16string result;
	result.reserve(input.size());
	bool lastWasSlash = false;
	for (char16_t ch : input) {
		char16_t normalized = ch;
		if (normalized == u'\\') {
			if (!result.empty() && !lastWasSlash) {
				result.push_back(u'\\');
			}
			lastWasSlash = true;
			continue;
		}
		lastWasSlash = false;
		uint16_t lowered = wcharToLower(static_cast<uint16_t>(normalized));
		result.push_back(static_cast<char16_t>(lowered));
	}
	while (!result.empty() && result.back() == u'\\') {
		result.pop_back();
	}
	auto it = result.begin();
	while (it != result.end() && *it == u'\\') {
		it = result.erase(it);
	}
	return result;
}

std::u16string canonicalizeKeySegment(LPCWSTR input) {
	if (!input) {
		return {};
	}
	std::u16string wide(reinterpret_cast<const char16_t *>(input), wstrlen(input));
	return canonicalizeKeySegment(wide);
}

Pin<RegistryKeyObject> predefinedHandleForValue(HKEY value) {
	static std::array<Pin<RegistryKeyObject>, kPredefinedKeyCount> g_predefinedHandles = [] {
		std::array<Pin<RegistryKeyObject>, kPredefinedKeyCount> arr;
		for (size_t i = 0; i < kPredefinedKeyCount; ++i) {
			arr[i] = make_pin<RegistryKeyObject>();
			arr[i]->canonicalPath = canonicalizeKeySegment(std::u16string(kPredefinedKeyInfos[i].name));
			arr[i]->predefined = true;
		}
		return arr;
	}();
	for (size_t i = 0; i < kPredefinedKeyCount; ++i) {
		if (kPredefinedKeyInfos[i].value == value) {
			return g_predefinedHandles[i].clone();
		}
	}
	return {};
}

Pin<RegistryKeyObject> handleDataFromHKeyLocked(HKEY hKey) {
	if (hKey == NO_HANDLE) {
		return {};
	}
	if (auto predefined = predefinedHandleForValue(hKey)) {
		return predefined;
	}
	auto obj = wibo::handles().getAs<RegistryKeyObject>(hKey);
	if (!obj) {
		return {};
	}
	return obj;
}

class RegistryLastErrorGuard {
	DWORD saved = kernel32::getLastError();

  public:
	~RegistryLastErrorGuard() { kernel32::setLastError(saved); }
};

LSTATUS providerRegistrySubkeys(const RegistryKeyObject &key, std::vector<RegistrySubkey> &entries) {
	const auto identity = providerCacheKey(key.canonicalPath, key.providerView);
	{
		std::lock_guard lock(g_registryMutex);
		if (auto cached = g_providerSubkeys.find(identity); cached != g_providerSubkeys.end()) {
			entries = cached->second;
			return ERROR_SUCCESS;
		}
	}
	std::string path;
	if (!wibo::provider::encodeUtf8(key.canonicalPath, path))
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"registry-subkeys", path, key.providerView}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status) || status < 0)
		return ERROR_INVALID_DATA;
	if (status != ERROR_SUCCESS)
		return reader.done() ? status : ERROR_INVALID_DATA;
	uint32_t count = 0;
	if (!reader.number(count) || count > 4096)
		return ERROR_INVALID_DATA;
	std::vector<RegistrySubkey> captured;
	std::unordered_set<std::u16string> names;
	captured.reserve(count);
	for (uint32_t index = 0; index < count; ++index) {
		RegistrySubkey entry;
		if (!reader.text(entry.name) || entry.name.empty() || entry.name.size() > 255 ||
			entry.name.find(u'\0') != std::u16string::npos || entry.name.find(u'\\') != std::u16string::npos ||
			!reader.text(entry.className) || entry.className.size() > 32767 ||
			entry.className.find(u'\0') != std::u16string::npos || !reader.number(entry.lastWriteTime.dwLowDateTime) ||
			!reader.number(entry.lastWriteTime.dwHighDateTime) ||
			!names.insert(canonicalizeValueName(reinterpret_cast<LPCWSTR>(entry.name.c_str()))).second)
			return ERROR_INVALID_DATA;
		captured.push_back(std::move(entry));
	}
	if (!reader.done())
		return ERROR_INVALID_DATA;
	{
		std::lock_guard lock(g_registryMutex);
		auto [cached, inserted] = g_providerSubkeys.try_emplace(identity, std::move(captured));
		(void)inserted;
		entries = cached->second;
	}
	return ERROR_SUCCESS;
}

LSTATUS enumerateRegistrySubkey(HKEY key, DWORD index, RegistrySubkey &result) {
	Pin<RegistryKeyObject> handle;
	bool local = false;
	{
		std::lock_guard lock(g_registryMutex);
		if (auto predefined = predefinedHandleForValue(key)) {
			handle = std::move(predefined);
		} else {
			HandleMeta metadata{};
			handle = wibo::handles().getAs<RegistryKeyObject>(key, &metadata);
			if (!handle)
				return ERROR_INVALID_HANDLE;
			if (!(metadata.grantedAccess & kKeyEnumerateSubkeys))
				return ERROR_ACCESS_DENIED;
		}
		local = g_existingKeys.contains(localCacheKey(handle->canonicalPath, handle->providerView));
	}
	std::vector<RegistrySubkey> nativeEntries;
	if (wibo::provider::configured()) {
		LSTATUS status = providerRegistrySubkeys(*handle, nativeEntries);
		if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && local))
			return status;
	} else if (!local) {
		// An uncaptured external key is not an evidenced empty registry key.
		return ERROR_NOT_SUPPORTED;
	}
	std::map<std::u16string, RegistrySubkey> entries;
	for (auto &entry : nativeEntries) {
		const auto name = canonicalizeValueName(reinterpret_cast<LPCWSTR>(entry.name.c_str()));
		entries.emplace(name, std::move(entry));
	}
	{
		std::lock_guard lock(g_registryMutex);
		const auto prefix = handle->canonicalPath + u'\\';
		for (auto &[name, entry] : entries) {
			if (auto written = g_localWriteTimes.find(localCacheKey(prefix + name, handle->providerView));
				written != g_localWriteTimes.end())
				entry.lastWriteTime = written->second;
		}
		for (const auto &[identity, entry] : g_localKeys) {
			if (!entry.canonicalPath.starts_with(prefix))
				continue;
			const auto relative = std::u16string_view(entry.canonicalPath).substr(prefix.size());
			if (relative.empty() || relative.find(u'\\') != std::u16string_view::npos ||
				identity != localCacheKey(entry.canonicalPath, handle->providerView))
				continue;
			entries.insert_or_assign(std::u16string(relative), entry.information);
		}
	}
	if (index >= entries.size())
		return ERROR_NO_MORE_ITEMS;
	auto selected = entries.begin();
	std::advance(selected, index);
	result = selected->second;
	return ERROR_SUCCESS;
}

LSTATUS registryAnsiString(const std::u16string &text, std::string &result) {
	if (std::all_of(text.begin(), text.end(), [](char16_t ch) { return ch <= 0x7f; })) {
		result.assign(text.begin(), text.end());
		return ERROR_SUCCESS;
	}
	if (text.size() > 16320 || !wibo::provider::configured())
		return ERROR_NOT_SUPPORTED;
	{
		std::lock_guard lock(g_registryMutex);
		if (auto cached = g_registryAnsiCache.find(text); cached != g_registryAnsiCache.end()) {
			result = cached->second;
			return ERROR_SUCCESS;
		}
	}
	const auto encoded = wibo::provider::encodeBytes(
		std::string_view(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(WCHAR)));
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"registry-ansi-string", encoded}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	if (status == wibo::provider::kUnavailable)
		return reader.done() ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA;
	if (status < 0)
		return ERROR_INVALID_DATA;
	if (status != ERROR_SUCCESS)
		return reader.done() ? status : ERROR_INVALID_DATA;
	std::vector<uint8_t> bytes;
	if (!reader.bytes(bytes) || bytes.empty() || bytes.size() > wibo::provider::kMaxResponse - 16 ||
		std::find(bytes.begin(), bytes.end(), uint8_t(0)) != bytes.end() || !reader.done())
		return ERROR_INVALID_DATA;
	std::string captured(bytes.begin(), bytes.end());
	{
		std::lock_guard lock(g_registryMutex);
		auto [cached, inserted] = g_registryAnsiCache.try_emplace(text, std::move(captured));
		(void)inserted;
		result = cached->second;
	}
	return ERROR_SUCCESS;
}

LSTATUS enumerateRegistryKey(HKEY key, DWORD index, void *name, DWORD *length, DWORD *reserved, void *keyClass,
							 DWORD *classLength, FILETIME *time, bool ansi) {
	if (reserved || !name || !length)
		return ERROR_INVALID_PARAMETER;
	RegistrySubkey entry;
	const LSTATUS status = enumerateRegistrySubkey(key, index, entry);
	if (status != ERROR_SUCCESS)
		return status;
	std::string ansiName, ansiClass;
	if (ansi) {
		LSTATUS converted = registryAnsiString(entry.name, ansiName);
		if (converted != ERROR_SUCCESS)
			return converted;
		if (classLength) {
			converted = registryAnsiString(entry.className, ansiClass);
			if (converted != ERROR_SUCCESS)
				return converted;
		}
	}
	const size_t nameSize = ansi ? ansiName.size() : entry.name.size();
	const size_t classSize = ansi ? ansiClass.size() : entry.className.size();
	// Native enumeration exposes the key timestamp even when a string buffer is short.
	if (time)
		*time = entry.lastWriteTime;
	if (nameSize >= *length || (keyClass && classLength && classSize >= *classLength))
		return kErrorMoreData;
	if (ansi)
		std::memcpy(name, ansiName.c_str(), nameSize + 1);
	else
		std::memcpy(name, entry.name.c_str(), (nameSize + 1) * sizeof(WCHAR));
	*length = static_cast<DWORD>(nameSize);
	if (classLength) {
		*classLength = static_cast<DWORD>(classSize);
		if (keyClass) {
			if (ansi)
				std::memcpy(keyClass, ansiClass.c_str(), classSize + 1);
			else
				std::memcpy(keyClass, entry.className.c_str(), (classSize + 1) * sizeof(WCHAR));
		}
	}
	return ERROR_SUCCESS;
}

bool isPredefinedKeyHandle(HKEY hKey) {
	return std::any_of(std::begin(kPredefinedKeyInfos), std::end(kPredefinedKeyInfos),
					   [hKey](const PredefinedKeyInfo &info) { return info.value == hKey; });
}

LSTATUS setRegistryValue(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size, bool ansi) {
	if (reserved || (!data && size)) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::mutex> lock(g_registryMutex);
	auto handle = handleDataFromHKeyLocked(key);
	if (!handle) {
		return ERROR_INVALID_HANDLE;
	}
	RegistryValue value;
	value.type = type;
	if (name)
		value.name.assign(reinterpret_cast<const char16_t *>(name), wstrlen(name));
	if (ansi && isRegistryString(type)) {
		if (size > std::numeric_limits<DWORD>::max() / sizeof(WCHAR)) {
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		// Match the existing shim's byte-to-U+00xx ACP mapping, with an
		// explicit byte count so embedded MULTI_SZ separators are retained.
		// General Windows code-page conversion remains unsupported.
		value.data.resize(static_cast<size_t>(size) * sizeof(WCHAR));
		for (size_t i = 0; i < size; ++i) {
			value.data[2 * i] = data[i];
			value.data[2 * i + 1] = 0;
		}
	} else if (size) {
		value.data.assign(data, data + size);
	}
	const auto canonicalName = canonicalizeValueName(name);
	auto &values = g_registryValues[localCacheKey(handle->canonicalPath, handle->providerView)];
	if (auto existing = values.find(canonicalName); existing != values.end()) {
		value.name = existing->second.name;
	} else if (auto snapshot = g_providerSnapshots.find(providerCacheKey(handle->canonicalPath, handle->providerView));
			   snapshot != g_providerSnapshots.end()) {
		if (auto existing = snapshot->second.find(canonicalName); existing != snapshot->second.end())
			value.name = existing->second.name;
	}
	values.insert_or_assign(canonicalName, std::move(value));
	FILETIME written{};
	kernel32::GetSystemTimeAsFileTime(&written);
	const auto identity = localCacheKey(handle->canonicalPath, handle->providerView);
	g_localWriteTimes.insert_or_assign(identity, written);
	if (auto local = g_localKeys.find(identity); local != g_localKeys.end())
		local->second.information.lastWriteTime = written;
	return ERROR_SUCCESS;
}

LSTATUS readRegistryValue(HKEY key, LPCWSTR name, RegistryValue &value) {
	std::lock_guard<std::mutex> lock(g_registryMutex);
	auto handle = handleDataFromHKeyLocked(key);
	if (!handle) {
		return ERROR_INVALID_HANDLE;
	}
	const auto canonicalName = canonicalizeValueName(name);
	auto keyValues = g_registryValues.find(localCacheKey(handle->canonicalPath, handle->providerView));
	if (keyValues != g_registryValues.end()) {
		auto entry = keyValues->second.find(canonicalName);
		if (entry != keyValues->second.end()) {
			value = entry->second;
			return ERROR_SUCCESS;
		}
	}
	return providerQuery(*handle, canonicalName, value);
}

LSTATUS writeRegistryValue(const RegistryValue &value, LPDWORD type, BYTE *data, LPDWORD size, bool ansi) {
	const bool narrowString = ansi && isRegistryString(value.type);
	const DWORD required = static_cast<DWORD>(narrowString ? value.data.size() / sizeof(WCHAR) : value.data.size());
	const DWORD capacity = data ? *size : 0;
	if (type) {
		*type = value.type;
	}
	if (size) {
		*size = required;
	}
	if (!data) {
		return ERROR_SUCCESS;
	}
	if (capacity < required) {
		return kErrorMoreData;
	}
	if (narrowString) {
		// The existing ACP facade narrows UTF-16 to its low byte. Full
		// Unicode-to-ACP conversion is not claimed by this implementation.
		for (size_t i = 0; i < required; ++i) {
			data[i] = value.data[2 * i];
		}
	} else if (required) {
		std::memcpy(data, value.data.data(), required);
	}
	return ERROR_SUCCESS;
}

LSTATUS queryRegistryValue(HKEY key, LPCWSTR name, const DWORD *reserved, LPDWORD type, BYTE *data, LPDWORD size,
						   bool ansi) {
	if (reserved || (data && !size)) {
		return ERROR_INVALID_PARAMETER;
	}
	RegistryValue value;
	const LSTATUS status = readRegistryValue(key, name, value);
	if (status != ERROR_SUCCESS)
		return status;
	return writeRegistryValue(value, type, data, size, ansi);
}

LSTATUS enumerateRegistryValue(HKEY key, DWORD index, RegistryValue &value) {
	std::lock_guard lock(g_registryMutex);
	auto handle = handleDataFromHKeyLocked(key);
	if (!handle)
		return ERROR_INVALID_HANDLE;
	std::map<std::u16string_view, const RegistryValue *> entries;
	if (wibo::provider::configured()) {
		const LSTATUS opened = providerOpen(handle->canonicalPath, handle->providerView);
		if (opened == ERROR_SUCCESS) {
			auto snapshot = g_providerSnapshots.find(providerCacheKey(handle->canonicalPath, handle->providerView));
			if (snapshot == g_providerSnapshots.end())
				return ERROR_NOT_SUPPORTED;
			for (const auto &[name, entry] : snapshot->second)
				entries.emplace(name, &entry);
		} else if (opened != ERROR_FILE_NOT_FOUND ||
				   !g_existingKeys.contains(localCacheKey(handle->canonicalPath, handle->providerView))) {
			return opened;
		}
	}
	if (auto local = g_registryValues.find(localCacheKey(handle->canonicalPath, handle->providerView));
		local != g_registryValues.end()) {
		for (const auto &[name, entry] : local->second)
			entries.insert_or_assign(name, &entry);
	}
	if (index >= entries.size())
		return ERROR_NO_MORE_ITEMS;
	auto selected = entries.begin();
	std::advance(selected, index);
	value = *selected->second;
	return ERROR_SUCCESS;
}

constexpr DWORD kRrfTypeMask = 0xFFFF;
constexpr DWORD kRrfExpandString = 0x4;
constexpr DWORD kRrfDword = 0x18;
constexpr DWORD kRrfQword = 0x48;
constexpr DWORD kRrfView64 = 0x10000;
constexpr DWORD kRrfView32 = 0x20000;
constexpr DWORD kRrfNoExpand = 0x10000000;
constexpr DWORD kRrfZeroOnFailure = 0x20000000;
constexpr LSTATUS kErrorDatatypeMismatch = 1629;
constexpr LSTATUS kErrorUnsupportedType = 1630;

DWORD registryTypeFlag(DWORD type) {
	switch (type) {
	case 0:
		return 0x1; // REG_NONE
	case 1:
		return 0x2; // REG_SZ
	case 2:
		return 0x4; // REG_EXPAND_SZ
	case 3:
		return 0x8; // REG_BINARY
	case 4:
		return 0x10; // REG_DWORD
	case 7:
		return 0x20; // REG_MULTI_SZ
	case 11:
		return 0x40; // REG_QWORD
	default:
		return 0;
	}
}

LSTATUS prepareRegistryString(RegistryValue &value, bool expand) {
	if (!isRegistryString(value.type))
		return ERROR_SUCCESS;
	if (value.data.size() % sizeof(WCHAR))
		return ERROR_INVALID_DATA;
	if (value.data.empty() || value.data[value.data.size() - 2] || value.data.back())
		value.data.resize(value.data.size() + sizeof(WCHAR), 0);
	if (value.type != kRegExpandSz || !expand)
		return ERROR_SUCCESS;
	std::u16string source(value.data.size() / sizeof(WCHAR), u'\0');
	std::memcpy(source.data(), value.data.data(), value.data.size());
	DWORD required = kernel32::ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(source.c_str()), nullptr, 0);
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		if (!required)
			return static_cast<LSTATUS>(kernel32::getLastError());
		if (required > std::numeric_limits<DWORD>::max() / sizeof(WCHAR))
			return ERROR_NOT_ENOUGH_MEMORY;
		std::vector<WCHAR> expanded(required);
		DWORD result =
			kernel32::ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(source.c_str()), expanded.data(), required);
		if (!result)
			return static_cast<LSTATUS>(kernel32::getLastError());
		if (result <= required) {
			const auto *bytes = reinterpret_cast<const BYTE *>(expanded.data());
			value.data.assign(bytes, bytes + static_cast<size_t>(result) * sizeof(WCHAR));
			value.type = kRegSz;
			return ERROR_SUCCESS;
		}
		required = result;
	}
	return kErrorMoreData;
}

LSTATUS getRegistryValue(HKEY key, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
	RegistryValue value;
	LSTATUS status = readRegistryValue(key, name, value);
	if (status != ERROR_SUCCESS)
		return status;
	status = prepareRegistryString(value, !(flags & kRrfNoExpand));
	if (status != ERROR_SUCCESS)
		return status;
	if (value.data.size() > std::numeric_limits<DWORD>::max())
		return ERROR_NOT_ENOUGH_MEMORY;
	const DWORD capacity = data ? *size : 0;
	const DWORD required = static_cast<DWORD>(value.data.size());
	if (type)
		*type = value.type;
	if (size)
		*size = required;
	const DWORD allowed = flags & kRrfTypeMask;
	if (allowed != kRrfTypeMask && !(allowed & registryTypeFlag(value.type)))
		return kErrorUnsupportedType;
	if (value.type == 3 && ((allowed == kRrfDword && required != sizeof(DWORD)) ||
							(allowed == kRrfQword && required != sizeof(ULONGLONG))))
		return kErrorDatatypeMismatch;
	if (!data)
		return ERROR_SUCCESS;
	if (capacity < required)
		return kErrorMoreData;
	if (required)
		std::memcpy(data, value.data.data(), required);
	return ERROR_SUCCESS;
}

} // namespace

namespace advapi32 {

LSTATUS WINAPI RegGetValueW(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data,
							LPDWORD size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegGetValueW(%p, %p, %p, 0x%x, %p, %p, %p)\n", key, subkey, value, flags, type, data, size);
	const DWORD savedError = kernel32::getLastError();
	const DWORD capacity = data && size ? *size : 0;
	const auto status = [&]() -> LSTATUS {
		const DWORD allowed = flags & kRrfTypeMask;
		if ((data && !size) || (flags & ~(kRrfTypeMask | kRrfView64 | kRrfView32 | kRrfNoExpand | kRrfZeroOnFailure)) ||
			(flags & (kRrfView64 | kRrfView32)) == (kRrfView64 | kRrfView32) ||
			(!(flags & kRrfNoExpand) && (allowed & kRrfExpandString) && allowed != kRrfTypeMask))
			return ERROR_INVALID_PARAMETER;
		if (!subkey || !*subkey)
			return getRegistryValue(key, value, flags, type, data, size);
		HKEY opened = NO_HANDLE;
		REGSAM access = 0x1; // KEY_QUERY_VALUE
		if (flags & kRrfView64)
			access |= KEY_WOW64_64KEY;
		if (flags & kRrfView32)
			access |= KEY_WOW64_32KEY;
		LSTATUS result = RegOpenKeyExW(key, subkey, 0, access, &opened);
		if (result == ERROR_SUCCESS) {
			result = getRegistryValue(opened, value, flags, type, data, size);
			RegCloseKey(opened);
		}
		return result;
	}();
	if (status != ERROR_SUCCESS && (flags & kRrfZeroOnFailure) && capacity)
		std::memset(data, 0, capacity);
	kernel32::setLastError(savedError);
	return status;
}

LSTATUS WINAPI RegCreateKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCreateKeyW(%p, %p, %p)\n", hKey, lpSubKey, phkResult);
	const DWORD savedError = kernel32::getLastError();
	const LSTATUS status =
		RegCreateKeyExW(hKey, lpSubKey, 0, nullptr, 0, kLegacyOpenAccess, nullptr, phkResult, nullptr);
	kernel32::setLastError(savedError);
	return status;
}

LSTATUS WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
							   REGSAM samDesired, void *lpSecurityAttributes, PHKEY phkResult,
							   LPDWORD lpdwDisposition) {
	HOST_CONTEXT_GUARD();
	std::string subKeyString = lpSubKey ? wideStringToString(lpSubKey) : std::string("(null)");
	std::string classString = lpClass ? wideStringToString(lpClass) : std::string("(null)");
	DEBUG_LOG("RegCreateKeyExW(%p, %s, %u, %s, 0x%x, 0x%x, %p, %p, %p)\n", hKey, subKeyString.c_str(), Reserved,
			  classString.c_str(), dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
	(void)lpSecurityAttributes;
	if (!phkResult) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	*phkResult = NO_HANDLE;
	if (Reserved != 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (dwOptions != 0) {
		DEBUG_LOG("RegCreateKeyExW: unsupported options 0x%x\n", dwOptions);
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	REGSAM sanitizedAccess = 0;
	const LSTATUS accessStatus = registryAccess(samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY), sanitizedAccess);
	if (accessStatus != ERROR_SUCCESS)
		return accessStatus;
	std::lock_guard<std::mutex> lock(g_registryMutex);
	Pin<RegistryKeyObject> baseHandle = handleDataFromHKeyLocked(hKey);
	if (!baseHandle) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	std::string providerView;
	const LSTATUS viewStatus = registryView(samDesired, baseHandle->providerView, providerView);
	if (viewStatus != ERROR_SUCCESS)
		return viewStatus;
	std::u16string targetPath = baseHandle->canonicalPath;
	const std::u16string requested =
		lpSubKey ? std::u16string(reinterpret_cast<const char16_t *>(lpSubKey), wstrlen(lpSubKey)) : std::u16string{};
	const std::u16string keyClass =
		lpClass ? std::u16string(reinterpret_cast<const char16_t *>(lpClass), wstrlen(lpClass)) : std::u16string{};
	bool existed = true;
	bool parentLocal = g_existingKeys.contains(localCacheKey(targetPath, providerView));
	FILETIME now{};
	kernel32::GetSystemTimeAsFileTime(&now);
	for (size_t start = 0; start < requested.size();) {
		const size_t end = requested.find(u'\\', start);
		const auto name = requested.substr(start, end == std::u16string::npos ? requested.size() - start : end - start);
		start = end == std::u16string::npos ? requested.size() : end + 1;
		if (name.empty())
			continue;
		const auto parentPath = targetPath;
		targetPath.push_back(u'\\');
		targetPath += canonicalizeKeySegment(name);
		const auto identity = localCacheKey(targetPath, providerView);
		existed = g_existingKeys.contains(identity);
		if (!existed && !parentLocal && wibo::provider::configured()) {
			const LSTATUS nativeStatus = providerOpen(targetPath, providerView);
			if (nativeStatus == ERROR_SUCCESS)
				existed = true;
			else if (nativeStatus != ERROR_FILE_NOT_FOUND)
				return nativeStatus;
		}
		if (!existed) {
			g_existingKeys.insert(identity);
			g_localKeys.emplace(identity, LocalRegistryKey{targetPath, {name, keyClass, now}});
			if (auto parent = g_localKeys.find(localCacheKey(parentPath, providerView)); parent != g_localKeys.end())
				parent->second.information.lastWriteTime = now;
		}
		parentLocal = g_existingKeys.contains(identity);
	}
	if (lpdwDisposition)
		*lpdwDisposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
	auto obj = make_pin<RegistryKeyObject>(std::move(targetPath));
	obj->providerView = std::move(providerView);
	auto handle = wibo::handles().alloc(std::move(obj), sanitizedAccess, 0);
	*phkResult = reinterpret_cast<HKEY>(handle);
	return ERROR_SUCCESS;
}

LSTATUS WINAPI RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
							   REGSAM samDesired, void *lpSecurityAttributes, PHKEY phkResult,
							   LPDWORD lpdwDisposition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCreateKeyExA(%p, %s, %u, %s, 0x%x, 0x%x, %p, %p, %p)\n", hKey, lpSubKey ? lpSubKey : "(null)",
			  Reserved, lpClass ? lpClass : "(null)", dwOptions, samDesired, lpSecurityAttributes, phkResult,
			  lpdwDisposition);
	std::vector<uint16_t> subKeyWideStorage;
	if (lpSubKey) {
		subKeyWideStorage = stringToWideString(lpSubKey);
	}
	std::vector<uint16_t> classWideStorage;
	if (lpClass) {
		classWideStorage = stringToWideString(lpClass);
	}
	return RegCreateKeyExW(hKey, lpSubKey ? reinterpret_cast<LPCWSTR>(subKeyWideStorage.data()) : nullptr, Reserved,
						   lpClass ? reinterpret_cast<LPWSTR>(classWideStorage.data()) : nullptr, dwOptions, samDesired,
						   lpSecurityAttributes, phkResult, lpdwDisposition);
}

LSTATUS WINAPI RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegOpenKeyA(%p, %s, %p)\n", hKey, lpSubKey ? lpSubKey : "(null)", phkResult);
	if (!phkResult) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!lpSubKey || !lpSubKey[0]) {
		// The legacy API returns this exact handle, including ordinary keys.
		*phkResult = hKey;
		return ERROR_SUCCESS;
	}
	const DWORD savedError = kernel32::getLastError();
	const LSTATUS status = RegOpenKeyExA(hKey, lpSubKey, 0, kLegacyOpenAccess, phkResult);
	// Registry status is returned directly; the legacy call preserves last error
	// even though the existing RegOpenKeyEx implementation changes it on failure.
	kernel32::setLastError(savedError);
	return status;
}

LSTATUS WINAPI RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegOpenKeyW(%p, %p, %p)\n", hKey, lpSubKey, phkResult);
	if (!phkResult) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!lpSubKey || !lpSubKey[0]) {
		*phkResult = hKey;
		return ERROR_SUCCESS;
	}
	const DWORD savedError = kernel32::getLastError();
	const LSTATUS status = RegOpenKeyExW(hKey, lpSubKey, 0, kLegacyOpenAccess, phkResult);
	kernel32::setLastError(savedError);
	return status;
}

LSTATUS WINAPI RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	std::string subKeyString = lpSubKey ? wideStringToString(lpSubKey) : std::string("(null)");
	DEBUG_LOG("RegOpenKeyExW(%p, %s, %u, 0x%x, %p)\n", hKey, subKeyString.c_str(), ulOptions, samDesired, phkResult);
	if (!phkResult) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	*phkResult = NO_HANDLE;
	if ((ulOptions & ~REG_OPTION_OPEN_LINK) != 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (ulOptions & REG_OPTION_OPEN_LINK) {
		DEBUG_LOG("RegOpenKeyExW: ignoring REG_OPTION_OPEN_LINK\n");
	}
	REGSAM sanitizedAccess = 0;
	const LSTATUS accessStatus = registryAccess(samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY), sanitizedAccess);
	if (accessStatus != ERROR_SUCCESS)
		return accessStatus;
	std::lock_guard<std::mutex> lock(g_registryMutex);
	Pin<RegistryKeyObject> baseHandle = handleDataFromHKeyLocked(hKey);
	if (!baseHandle) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	std::u16string targetPath = baseHandle->canonicalPath;
	if (lpSubKey && lpSubKey[0] != 0) {
		std::u16string subComponent = canonicalizeKeySegment(lpSubKey);
		if (!subComponent.empty()) {
			if (!targetPath.empty()) {
				targetPath.push_back(u'\\');
			}
			targetPath.append(subComponent);
		}
	}
	if (targetPath.empty()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	std::string providerView;
	const LSTATUS viewStatus = registryView(samDesired, baseHandle->providerView, providerView);
	if (viewStatus != ERROR_SUCCESS)
		return viewStatus;
	if (!g_existingKeys.contains(localCacheKey(targetPath, providerView))) {
		const LSTATUS status = providerOpen(targetPath, providerView);
		if (status != ERROR_SUCCESS)
			return status;
	}
	if (!lpSubKey || lpSubKey[0] == 0) {
		if (baseHandle->predefined) {
			*phkResult = hKey;
			return ERROR_SUCCESS;
		}
	}
	auto obj = make_pin<RegistryKeyObject>(std::move(targetPath));
	obj->providerView = std::move(providerView);
	auto handle = wibo::handles().alloc(std::move(obj), sanitizedAccess, 0);
	*phkResult = reinterpret_cast<HKEY>(handle);
	return ERROR_SUCCESS;
}

LSTATUS WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegOpenKeyExA(%p, %s, %u, 0x%x, %p)\n", hKey, lpSubKey ? lpSubKey : "(null)", ulOptions, samDesired,
			  phkResult);
	LPCWSTR widePtr = nullptr;
	std::vector<uint16_t> wideStorage;
	if (lpSubKey) {
		wideStorage = stringToWideString(lpSubKey);
		widePtr = reinterpret_cast<LPCWSTR>(wideStorage.data());
	}
	return RegOpenKeyExW(hKey, widePtr, ulOptions, samDesired, phkResult);
}

LSTATUS WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData,
							  DWORD cbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegSetValueExW(%p, %p, %u, %u, %p, %u)\n", hKey, lpValueName, Reserved, dwType, lpData, cbData);
	return setRegistryValue(hKey, lpValueName, Reserved, dwType, lpData, cbData, false);
}

LSTATUS WINAPI RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData,
							  DWORD cbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegSetValueExA(%p, %s, %u, %u, %p, %u)\n", hKey, lpValueName ? lpValueName : "(default)", Reserved,
			  dwType, lpData, cbData);
	const auto name = stringToWideString(lpValueName);
	return setRegistryValue(hKey, name.data(), Reserved, dwType, lpData, cbData, true);
}

LSTATUS WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, BYTE *lpData,
								LPDWORD lpcbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegQueryValueExW(%p, %p, %p, %p, %p, %p)\n", hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
	return queryRegistryValue(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData, false);
}

LSTATUS WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, BYTE *lpData,
								LPDWORD lpcbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegQueryValueExA(%p, %s, %p, %p, %p, %p)\n", hKey, lpValueName ? lpValueName : "(default)", lpReserved,
			  lpType, lpData, lpcbData);
	const auto name = stringToWideString(lpValueName);
	return queryRegistryValue(hKey, name.data(), lpReserved, lpType, lpData, lpcbData, true);
}

LSTATUS WINAPI RegEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD length, LPDWORD reserved, LPDWORD type,
							 BYTE *data, LPDWORD size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumValueW(%p, %u, %p, %p, %p, %p, %p, %p)\n", key, index, name, length, reserved, type, data, size);
	if (!name || !length || reserved || (data && !size))
		return ERROR_INVALID_PARAMETER;
	RegistryValue value;
	const LSTATUS status = enumerateRegistryValue(key, index, value);
	if (status != ERROR_SUCCESS)
		return status;
	if (*length <= value.name.size()) {
		writeRegistryValue(value, type, nullptr, size, false);
		return kErrorMoreData;
	}
	std::memcpy(name, value.name.c_str(), (value.name.size() + 1) * sizeof(WCHAR));
	*length = static_cast<DWORD>(value.name.size());
	return writeRegistryValue(value, type, data, size, false);
}

LSTATUS WINAPI RegEnumKeyW(HKEY key, DWORD index, LPWSTR name, DWORD capacity) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyW(%p, %u, %p, %u)\n", key, index, name, capacity);
	RegistryLastErrorGuard errorGuard;
	return enumerateRegistryKey(key, index, name, &capacity, nullptr, nullptr, nullptr, nullptr, false);
}

LSTATUS WINAPI RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD capacity) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyA(%p, %u, %p, %u)\n", key, index, name, capacity);
	RegistryLastErrorGuard errorGuard;
	return enumerateRegistryKey(key, index, name, &capacity, nullptr, nullptr, nullptr, nullptr, true);
}

LSTATUS WINAPI RegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							 LPWSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExW(%p, %u, %p, %p, %p, %p, %p, %p)\n", hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass,
			  lpcchClass, lpftLastWriteTime);
	RegistryLastErrorGuard errorGuard;
	return enumerateRegistryKey(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime,
								false);
}

LSTATUS WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							 LPSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExA(%p, %u, %p, %p, %p, %p, %p, %p)\n", hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass,
			  lpcchClass, lpftLastWriteTime);
	RegistryLastErrorGuard errorGuard;
	return enumerateRegistryKey(hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime,
								true);
}

LSTATUS WINAPI RegCloseKey(HKEY hKey) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCloseKey(%p)\n", hKey);
	if (isPredefinedKeyHandle(hKey)) {
		return ERROR_SUCCESS;
	}
	auto object = wibo::handles().getAs<RegistryKeyObject>(hKey);
	if (!object)
		return ERROR_INVALID_HANDLE;
	return wibo::handles().release(hKey) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

} // namespace advapi32
