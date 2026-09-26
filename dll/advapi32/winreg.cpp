#include "winreg.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "kernel32/processenv.h"
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

	std::mutex m;
	std::u16string canonicalPath;
	bool closed = false;
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
	{HKEY_CLASSES_ROOT, u"HKEY_CLASSES_ROOT"},
	{HKEY_CURRENT_USER, u"HKEY_CURRENT_USER"},
	{HKEY_LOCAL_MACHINE, u"HKEY_LOCAL_MACHINE"},
	{HKEY_USERS, u"HKEY_USERS"},
	{HKEY_PERFORMANCE_DATA, u"HKEY_PERFORMANCE_DATA"},
	{HKEY_CURRENT_CONFIG, u"HKEY_CURRENT_CONFIG"},
};

constexpr size_t kPredefinedKeyCount = std::size(kPredefinedKeyInfos);
constexpr REGSAM kLegacyOpenAccess = 0x02000000; // MAXIMUM_ALLOWED

std::mutex g_registryMutex;
std::unordered_set<std::u16string> g_existingKeys;

struct RegistryValue {
	DWORD type = 0;
	std::vector<BYTE> data{};
	std::u16string name;
};

// Values belong to a key path, not an open handle. The registry is process-local;
// this does not add persistence, ACL checks, or separate WOW64 registry views.
using RegistryValues = std::unordered_map<std::u16string, RegistryValue>;
std::unordered_map<std::u16string, RegistryValues> g_registryValues;
constexpr LSTATUS kErrorInvalidData = 13;
constexpr DWORD kRegSz = 1;
constexpr DWORD kRegExpandSz = 2;
constexpr DWORD kRegMultiSz = 7;
constexpr LSTATUS kErrorMoreData = 234;

// Provider data is a read-only process snapshot. Guest writes remain in the
// existing local store and never modify the provider's environment.
std::unordered_map<std::u16string, LSTATUS> g_providerKeys;
std::unordered_map<std::u16string, RegistryValues> g_providerSnapshots;
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
		return kErrorInvalidData;
	RegistryValues values;
	if (status == ERROR_SUCCESS && snapshot) {
		uint32_t count = 0;
		if (!reader.number(count) || count > 4096)
			return kErrorInvalidData;
		for (uint32_t i = 0; i < count; ++i) {
			std::u16string name;
			RegistryValue value;
			if (!reader.text(name) || name.find(u'\0') != std::u16string::npos || !reader.number(value.type) ||
				!reader.bytes(value.data))
				return kErrorInvalidData;
			const auto canonicalName = canonicalizeValueName(reinterpret_cast<LPCWSTR>(name.c_str()));
			value.name = std::move(name);
			values.insert_or_assign(canonicalName, std::move(value));
		}
	}
	if (!reader.done())
		return kErrorInvalidData;
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
		return kErrorInvalidData;
	if (status == ERROR_SUCCESS) {
		if (!reader.number(value.type) || !reader.bytes(value.data))
			return kErrorInvalidData;
	}
	if (!reader.done())
		return kErrorInvalidData;
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
		char16_t normalized = (ch == u'/') ? u'\\' : ch;
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
	if (!obj || obj->closed) {
		return {};
	}
	return obj;
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
	auto &values = g_registryValues[handle->canonicalPath];
	if (auto existing = values.find(canonicalName); existing != values.end()) {
		value.name = existing->second.name;
	} else if (auto snapshot = g_providerSnapshots.find(providerCacheKey(handle->canonicalPath, handle->providerView));
			   snapshot != g_providerSnapshots.end()) {
		if (auto existing = snapshot->second.find(canonicalName); existing != snapshot->second.end())
			value.name = existing->second.name;
	}
	values.insert_or_assign(canonicalName, std::move(value));
	return ERROR_SUCCESS;
}

LSTATUS readRegistryValue(HKEY key, LPCWSTR name, RegistryValue &value) {
	std::lock_guard<std::mutex> lock(g_registryMutex);
	auto handle = handleDataFromHKeyLocked(key);
	if (!handle) {
		return ERROR_INVALID_HANDLE;
	}
	const auto canonicalName = canonicalizeValueName(name);
	auto keyValues = g_registryValues.find(handle->canonicalPath);
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
		} else if (opened != ERROR_FILE_NOT_FOUND || !g_existingKeys.contains(handle->canonicalPath)) {
			return opened;
		}
	}
	if (auto local = g_registryValues.find(handle->canonicalPath); local != g_registryValues.end()) {
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
		return kErrorInvalidData;
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
	const LSTATUS status = RegCreateKeyExW(hKey, lpSubKey, 0, nullptr, 0, kLegacyOpenAccess, nullptr, phkResult, nullptr);
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
	(void)lpClass;
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
	REGSAM sanitizedAccess = samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
	if (sanitizedAccess != samDesired) {
		DEBUG_LOG("RegCreateKeyExW: ignoring WOW64 access mask 0x%x\n", samDesired ^ sanitizedAccess);
	}
	std::lock_guard<std::mutex> lock(g_registryMutex);
	Pin<RegistryKeyObject> baseHandle = handleDataFromHKeyLocked(hKey);
	if (!baseHandle) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	std::u16string targetPath = baseHandle->canonicalPath;
	bool targetingBase = true;
	if (lpSubKey && lpSubKey[0] != 0) {
		std::u16string subComponent = canonicalizeKeySegment(lpSubKey);
		if (!subComponent.empty()) {
			targetingBase = false;
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
	bool existed = g_existingKeys.find(targetPath) != g_existingKeys.end();
	if (!existed) {
		g_existingKeys.insert(targetPath);
	}
	if (lpdwDisposition) {
		*lpdwDisposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
	}
	if (targetingBase) {
		*phkResult = hKey;
		return ERROR_SUCCESS;
	}
	auto obj = make_pin<RegistryKeyObject>(std::move(targetPath));
	auto handle = wibo::handles().alloc(std::move(obj), 0, 0);
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
	REGSAM sanitizedAccess = samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
	if (sanitizedAccess != samDesired) {
		DEBUG_LOG("RegOpenKeyExW: ignoring WOW64 access mask 0x%x\n", samDesired ^ sanitizedAccess);
	}
	(void)sanitizedAccess;
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
	std::string providerView = baseHandle->providerView;
	if (samDesired & KEY_WOW64_64KEY)
		providerView = "64";
	if (samDesired & KEY_WOW64_32KEY)
		providerView = "32";
	if ((samDesired & (KEY_WOW64_64KEY | KEY_WOW64_32KEY)) == (KEY_WOW64_64KEY | KEY_WOW64_32KEY))
		return ERROR_INVALID_PARAMETER;
	if (g_existingKeys.find(targetPath) == g_existingKeys.end()) {
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
	auto handle = wibo::handles().alloc(std::move(obj), 0, 0);
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

LSTATUS WINAPI RegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							   LPWSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExW(%p, %u, %p, %p, %p, %p, %p, %p)\n", hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass,
			  lpcchClass, lpftLastWriteTime);
	(void)hKey;
	(void)dwIndex;
	if (lpReserved) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (lpcchName) {
		*lpcchName = 0;
	}
	if (lpName && lpcchName && *lpcchName > 0) {
		lpName[0] = 0;
	}
	if (lpClass && lpcchClass && *lpcchClass > 0) {
		lpClass[0] = 0;
	}
	if (lpcchClass) {
		*lpcchClass = 0;
	}
	(void)lpftLastWriteTime;
	kernel32::setLastError(ERROR_NO_MORE_ITEMS);
	return ERROR_NO_MORE_ITEMS;
}

LSTATUS WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							   LPSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExA(%p, %u, %p, %p, %p, %p, %p, %p)\n", hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass,
			  lpcchClass, lpftLastWriteTime);
	(void)hKey;
	(void)dwIndex;
	if (lpReserved) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (lpcchName) {
		*lpcchName = 0;
	}
	if (lpName && lpcchName && *lpcchName > 0) {
		lpName[0] = '\0';
	}
	if (lpClass && lpcchClass && *lpcchClass > 0) {
		lpClass[0] = '\0';
	}
	if (lpcchClass) {
		*lpcchClass = 0;
	}
	(void)lpftLastWriteTime;
	kernel32::setLastError(ERROR_NO_MORE_ITEMS);
	return ERROR_NO_MORE_ITEMS;
}

LSTATUS WINAPI RegCloseKey(HKEY hKey) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCloseKey(%p)\n", hKey);
	if (isPredefinedKeyHandle(hKey)) {
		return ERROR_SUCCESS;
	}
	auto obj = wibo::handles().getAs<RegistryKeyObject>(hKey);
	if (!obj || obj->closed) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	return ERROR_SUCCESS;
}

} // namespace advapi32
