// Standalone Windows service adapter. The caller selects its execution environment.
// The build target supplies the compiler and system libraries.
#define CINTERFACE
#define COBJMACROS
#include "../src/security_descriptor.h"
#include <winsock2.h>
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <oleauto.h>
#include <wbemcli.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr size_t kMaxResponse = 8 * 1024 * 1024;

class Response {
	std::vector<BYTE> data;
	bool valid = true;

  public:
	void number(uint32_t value) {
		BYTE bytes[4] = {BYTE(value), BYTE(value >> 8), BYTE(value >> 16), BYTE(value >> 24)};
		append(bytes, 4);
	}
	void append(const void *source, size_t size) {
		if (size > kMaxResponse - data.size()) {
			valid = false;
			return;
		}
		if (size) {
			const auto *bytes = static_cast<const BYTE *>(source);
			data.insert(data.end(), bytes, bytes + size);
		}
	}
	void bytes(const void *source, size_t size) {
		number(static_cast<uint32_t>(size));
		append(source, size);
	}
	void header(uint32_t status) {
		number(0x50535957);
		number(1);
		number(status);
	}
	bool write() const { return valid && fwrite(data.data(), 1, data.size(), stdout) == data.size(); }
	bool good() const { return valid; }
};

struct Property {
	std::vector<BYTE> name;
	CIMTYPE type;
	LONG flavor;
	VARTYPE variantType;
	HRESULT status;
	std::vector<BYTE> value;
};
using Record = std::vector<Property>;

std::vector<BYTE> copyBytes(const void *source, size_t size) {
	const auto *bytes = static_cast<const BYTE *>(source);
	return size ? std::vector<BYTE>(bytes, bytes + size) : std::vector<BYTE>();
}

Property copyProperty(BSTR name, CIMTYPE type, LONG flavor, const VARIANT &value) {
	Property result{copyBytes(name, SysStringLen(name) * sizeof(WCHAR)), type, flavor, V_VT(&value), S_OK, {}};
	switch (V_VT(&value)) {
	case VT_EMPTY:
	case VT_NULL:
		break;
	case VT_BSTR:
		result.value = copyBytes(V_BSTR(&value), SysStringLen(V_BSTR(&value)) * sizeof(WCHAR));
		break;
	case VT_I1:
	case VT_UI1:
		result.value = copyBytes(&value.bVal, 1);
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		result.value = copyBytes(&value.iVal, 2);
		break;
	case VT_ERROR:
	case VT_I4:
	case VT_UI4:
	case VT_R4:
	case VT_INT:
	case VT_UINT:
		result.value = copyBytes(&value.lVal, 4);
		break;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
		result.value = copyBytes(&value.llVal, 8);
		break;
	default:
		result.status = WBEM_E_NOT_SUPPORTED;
		break;
	}
	return result;
}

bool ipAddressTable(bool ordered) {
	ULONG size = 0;
	DWORD status = GetIpAddrTable(nullptr, &size, ordered);
	std::vector<BYTE> storage;
	for (unsigned attempt = 0; status == ERROR_INSUFFICIENT_BUFFER && attempt != 3; ++attempt) {
		if (size < sizeof(DWORD) || size > kMaxResponse - 16) {
			status = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}
		storage.resize(size);
		status = GetIpAddrTable(reinterpret_cast<MIB_IPADDRTABLE *>(storage.data()), &size, ordered);
	}
	const auto *table = reinterpret_cast<const MIB_IPADDRTABLE *>(storage.data());
	static_assert(sizeof(MIB_IPADDRROW) == 24 && offsetof(MIB_IPADDRTABLE, table) == 4);
	if (status == NO_ERROR && (storage.size() < sizeof(DWORD) || table->dwNumEntries > 65536 ||
							   table->dwNumEntries > (storage.size() - sizeof(DWORD)) / sizeof(MIB_IPADDRROW)))
		status = ERROR_INVALID_DATA;
	Response response;
	response.header(status);
	if (status == NO_ERROR) {
		response.number(table->dwNumEntries);
		for (DWORD index = 0; index != table->dwNumEntries; ++index) {
			const auto &row = table->table[index];
			response.number(row.dwAddr);
			response.number(row.dwIndex);
			response.number(row.dwMask);
			response.number(row.dwBCastAddr);
			response.number(row.dwReasmSize);
			response.number(row.unused1 | (static_cast<DWORD>(row.wType) << 16));
		}
	}
	return response.write();
}

bool formatMessage(WCHAR **parameters, bool wide) {
	DWORD values[3]{};
	for (unsigned index = 0; index < 3; ++index) {
		WCHAR *end = nullptr;
		const unsigned long long value = wcstoull(parameters[index], &end, 10);
		if (!*parameters[index] || *end || value > 0xFFFFFFFFULL)
			return false;
		values[index] = static_cast<DWORD>(value);
	}
	const DWORD flags = values[0];
	Response response;
	if (!(flags & FORMAT_MESSAGE_FROM_SYSTEM) ||
		(flags & ~(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ARGUMENT_ARRAY |
				   FORMAT_MESSAGE_MAX_WIDTH_MASK))) {
		response.header(ERROR_INVALID_PARAMETER);
		return response.write();
	}
	void *buffer = nullptr;
	const DWORD allocationFlags = flags | FORMAT_MESSAGE_ALLOCATE_BUFFER;
	const DWORD count = wide ? FormatMessageW(allocationFlags, nullptr, values[1], values[2],
											  reinterpret_cast<WCHAR *>(&buffer), 0, nullptr)
							 : FormatMessageA(allocationFlags, nullptr, values[1], values[2],
											  reinterpret_cast<char *>(&buffer), 0, nullptr);
	response.header(count ? ERROR_SUCCESS : GetLastError());
	if (count)
		response.bytes(buffer, count * (wide ? sizeof(WCHAR) : 1));
	if (buffer)
		LocalFree(buffer);
	return response.write();
}

bool networkConnectivity(const WCHAR *contextText) {
	WCHAR *end = nullptr;
	const unsigned long context = wcstoul(contextText, &end, 10);
	HRESULT status = *contextText && !*end ? CoInitializeEx(nullptr, COINIT_MULTITHREADED) : E_INVALIDARG;
	const bool initialized = SUCCEEDED(status);
	INetworkListManager *manager = nullptr;
	NLM_CONNECTIVITY flags = NLM_CONNECTIVITY_DISCONNECTED;
	VARIANT_BOOL connected = VARIANT_FALSE, internet = VARIANT_FALSE;
	if (SUCCEEDED(status))
		status = CoCreateInstance(CLSID_NetworkListManager, nullptr, context, IID_INetworkListManager,
								  reinterpret_cast<void **>(&manager));
	if (SUCCEEDED(status))
		status = INetworkListManager_GetConnectivity(manager, &flags);
	if (SUCCEEDED(status))
		status = INetworkListManager_IsConnected(manager, &connected);
	if (SUCCEEDED(status))
		status = INetworkListManager_IsConnectedToInternet(manager, &internet);
	if (manager)
		INetworkListManager_Release(manager);
	if (initialized)
		CoUninitialize();
	Response response;
	response.header(status);
	if (SUCCEEDED(status)) {
		response.number(flags);
		response.number(connected != VARIANT_FALSE);
		response.number(internet != VARIANT_FALSE);
	}
	return response.write();
}

bool management(const WCHAR *spaceName, const WCHAR *queryText, WCHAR **security) {
	std::vector<Record> records;
	HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool initialized = SUCCEEDED(status);
	IWbemLocator *locator = nullptr;
	IWbemServices *services = nullptr;
	IEnumWbemClassObject *enumeration = nullptr;
	BSTR space = SysAllocString(spaceName);
	BSTR language = SysAllocString(L"WQL");
	BSTR query = queryText ? SysAllocString(queryText) : nullptr;
	if (SUCCEEDED(status) && (!space || !language || (queryText && !query))) {
		status = E_OUTOFMEMORY;
	}
	if (SUCCEEDED(status)) {
		status = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
								  reinterpret_cast<void **>(&locator));
	}
	if (SUCCEEDED(status)) {
		status = IWbemLocator_ConnectServer(locator, space, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT,
											nullptr, nullptr, &services);
	}
	if (SUCCEEDED(status) && security) {
		DWORD parameters[5];
		for (unsigned i = 0; i < 5; ++i) {
			WCHAR *end = nullptr;
			const unsigned long long value = wcstoull(security[i], &end, 10);
			if (!*security[i] || *end || value > 0xFFFFFFFFULL) {
				status = E_INVALIDARG;
				break;
			}
			parameters[i] = static_cast<DWORD>(value);
		}
		if (SUCCEEDED(status))
			status = CoSetProxyBlanket(reinterpret_cast<IUnknown *>(services), parameters[0], parameters[1], nullptr,
									   parameters[2], parameters[3], nullptr, parameters[4]);
	}
	if (SUCCEEDED(status) && queryText) {
		status = IWbemServices_ExecQuery(services, language, query,
										 WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumeration);
	}
	size_t responseSize = 16;
	while (SUCCEEDED(status) && enumeration) {
		IWbemClassObject *object = nullptr;
		ULONG returned = 0;
		status = IEnumWbemClassObject_Next(enumeration, 5000, 1, &object, &returned);
		if (status == WBEM_S_TIMEDOUT) {
			status = WBEM_E_TIMED_OUT;
		}
		if (FAILED(status) || !returned) {
			if (object)
				IWbemClassObject_Release(object);
			break;
		}
		Record record;
		status = IWbemClassObject_BeginEnumeration(object, 0);
		responseSize += 4;
		while (SUCCEEDED(status)) {
			BSTR name = nullptr;
			VARIANT value;
			VariantInit(&value);
			CIMTYPE type = 0;
			LONG flavor = 0;
			status = IWbemClassObject_Next(object, 0, &name, &value, &type, &flavor);
			if (status == WBEM_S_NO_MORE_DATA || FAILED(status)) {
				VariantClear(&value);
				SysFreeString(name);
				break;
			}
			auto property = copyProperty(name, type, flavor, value);
			responseSize += 24 + property.name.size() + property.value.size();
			VariantClear(&value);
			SysFreeString(name);
			if (responseSize > kMaxResponse || record.size() >= 4096) {
				status = WBEM_E_QUOTA_VIOLATION;
				break;
			}
			record.push_back(std::move(property));
		}
		if (status == WBEM_S_NO_MORE_DATA)
			status = S_OK;
		IWbemClassObject_EndEnumeration(object);
		IWbemClassObject_Release(object);
		if (FAILED(status))
			break;
		records.push_back(std::move(record));
		if (records.size() >= 4096) {
			status = WBEM_E_QUOTA_VIOLATION;
			break;
		}
	}
	if (SUCCEEDED(status))
		status = S_OK;
	if (FAILED(status))
		records.clear();
	Response response;
	response.header(status);
	response.number(static_cast<uint32_t>(records.size()));
	for (const auto &record : records) {
		response.number(static_cast<uint32_t>(record.size()));
		for (const auto &property : record) {
			response.bytes(property.name.data(), property.name.size());
			response.number(property.type);
			response.number(property.flavor);
			response.number(property.variantType);
			response.number(property.status);
			response.bytes(property.value.data(), property.value.size());
		}
	}
	if (enumeration)
		IEnumWbemClassObject_Release(enumeration);
	if (services)
		IWbemServices_Release(services);
	if (locator)
		IWbemLocator_Release(locator);
	SysFreeString(space);
	SysFreeString(language);
	SysFreeString(query);
	if (initialized)
		CoUninitialize();
	return response.write();
}

bool userName() {
	DWORD wideSize = 0, narrowSize = 0;
	GetUserNameW(nullptr, &wideSize);
	DWORD status = GetLastError();
	std::vector<WCHAR> wide;
	std::vector<char> narrow;
	if (status == ERROR_INSUFFICIENT_BUFFER && wideSize <= 32768) {
		wide.resize(wideSize);
		status = GetUserNameW(wide.data(), &wideSize) ? ERROR_SUCCESS : GetLastError();
	}
	if (status == ERROR_SUCCESS) {
		GetUserNameA(nullptr, &narrowSize);
		status = GetLastError();
		if (status == ERROR_INSUFFICIENT_BUFFER && narrowSize <= 32768) {
			narrow.resize(narrowSize);
			status = GetUserNameA(narrow.data(), &narrowSize) ? ERROR_SUCCESS : GetLastError();
		}
	}
	Response response;
	response.header(status);
	if (status == ERROR_SUCCESS) {
		response.bytes(wide.data(), wcslen(wide.data()) * sizeof(WCHAR));
		response.bytes(narrow.data(), strlen(narrow.data()));
	}
	return response.write();
}

bool decodeHex(const WCHAR *source, std::string &result, bool allowZero = false) {
	result.clear();
	while (*source) {
		unsigned value = 0;
		for (unsigned i = 0; i < 2; ++i) {
			WCHAR ch = *source++;
			unsigned digit;
			if (ch >= '0' && ch <= '9')
				digit = ch - '0';
			else if (ch >= 'a' && ch <= 'f')
				digit = ch - 'a' + 10;
			else
				return false;
			value = value * 16 + digit;
		}
		if (!value && !allowZero)
			return false;
		result.push_back(static_cast<char>(value));
	}
	return true;
}

bool account(const WCHAR *system, const WCHAR *name, bool ansi) {
	std::string narrowSystem, narrowName;
	if (ansi && (!decodeHex(system, narrowSystem) || !decodeHex(name, narrowName)))
		return false;
	DWORD sidSize = 0, domainSize = 0;
	SID_NAME_USE use = SidTypeUnknown;
	if (ansi)
		LookupAccountNameA(narrowSystem.empty() ? nullptr : narrowSystem.c_str(), narrowName.c_str(), nullptr, &sidSize,
						   nullptr, &domainSize, &use);
	else
		LookupAccountNameW(*system ? system : nullptr, name, nullptr, &sidSize, nullptr, &domainSize, &use);
	DWORD status = GetLastError();
	std::vector<BYTE> sid, domain;
	if (status == ERROR_INSUFFICIENT_BUFFER) {
		if (sidSize > SECURITY_MAX_SID_SIZE || domainSize > 32768)
			status = ERROR_NOT_ENOUGH_MEMORY;
		else {
			sid.resize(sidSize);
			domain.resize(domainSize * (ansi ? 1 : sizeof(WCHAR)));
			BOOL found;
			if (ansi)
				found = LookupAccountNameA(narrowSystem.empty() ? nullptr : narrowSystem.c_str(), narrowName.c_str(),
										   sid.data(), &sidSize, reinterpret_cast<char *>(domain.data()), &domainSize,
										   &use);
			else
				found = LookupAccountNameW(*system ? system : nullptr, name, sid.data(), &sidSize,
										   reinterpret_cast<WCHAR *>(domain.data()), &domainSize, &use);
			status = found ? ERROR_SUCCESS : GetLastError();
			if (found) {
				sid.resize(GetLengthSid(sid.data()));
				const size_t length = ansi ? strlen(reinterpret_cast<char *>(domain.data()))
										   : wcslen(reinterpret_cast<WCHAR *>(domain.data())) * sizeof(WCHAR);
				domain.resize(length);
			}
		}
	}
	Response response;
	response.header(status);
	if (status == ERROR_SUCCESS) {
		response.bytes(sid.data(), sid.size());
		response.bytes(domain.data(), domain.size());
		response.number(use);
	}
	return response.write();
}

bool setFileSecurity(const WCHAR *path, const WCHAR *information, const WCHAR *serialized, bool ansi) {
	WCHAR *end = nullptr;
	const unsigned long long parsed = wcstoull(information, &end, 10);
	if (!*information || *end || parsed > 0xFFFFFFFFULL)
		return false;
	std::string narrow, decoded;
	if ((ansi && !decodeHex(path, narrow)) || !decodeHex(serialized, decoded, true))
		return false;
	std::vector<BYTE> data(decoded.begin(), decoded.end());
	DWORD status = ERROR_INVALID_SECURITY_DESCR;
	if (wibo::security::validRelativeDescriptor(data) && IsValidSecurityDescriptor(data.data())) {
		const BOOL applied = ansi ? SetFileSecurityA(narrow.c_str(), static_cast<DWORD>(parsed), data.data())
								  : SetFileSecurityW(path, static_cast<DWORD>(parsed), data.data());
		status = applied ? ERROR_SUCCESS : GetLastError();
	}
	Response response;
	response.header(status);
	return response.write();
}

bool fileSecurity(const WCHAR *path, const WCHAR *information, bool ansi) {
	WCHAR *end = nullptr;
	const unsigned long long parsed = wcstoull(information, &end, 10);
	if (!*information || *end || parsed > 0xFFFFFFFFULL)
		return false;
	const DWORD requested = static_cast<DWORD>(parsed);
	std::string narrow;
	if (ansi && !decodeHex(path, narrow))
		return false;
	DWORD size = 0;
	if (ansi)
		GetFileSecurityA(narrow.c_str(), requested, nullptr, 0, &size);
	else
		GetFileSecurityW(path, requested, nullptr, 0, &size);
	DWORD status = GetLastError();
	std::vector<BYTE> descriptor;
	if (status == ERROR_INSUFFICIENT_BUFFER) {
		if (size > kMaxResponse - 16)
			status = ERROR_NOT_ENOUGH_MEMORY;
		else {
			descriptor.resize(size);
			BOOL found = ansi ? GetFileSecurityA(narrow.c_str(), requested, descriptor.data(), size, &size)
							  : GetFileSecurityW(path, requested, descriptor.data(), size, &size);
			status = found ? ERROR_SUCCESS : GetLastError();
			if (found) {
				descriptor.resize(size);
				if (!IsValidSecurityDescriptor(descriptor.data()))
					status = ERROR_INVALID_SECURITY_DESCR;
			}
		}
	}
	Response response;
	response.header(status);
	if (status == ERROR_SUCCESS)
		response.bytes(descriptor.data(), descriptor.size());
	return response.write();
}

struct BitmapData {
	BITMAP bitmap{};
	std::vector<BYTE> bytes;
	bool read(HBITMAP handle) {
		if (GetObjectW(handle, sizeof(bitmap), &bitmap) != sizeof(bitmap) || bitmap.bmWidth <= 0 ||
			bitmap.bmWidth > 4096 || bitmap.bmHeight <= 0 || bitmap.bmHeight > 8192 || bitmap.bmPlanes != 1 ||
			bitmap.bmBitsPixel > 32 || !bitmap.bmBitsPixel)
			return false;
		const size_t stride = ((size_t(bitmap.bmWidth) * bitmap.bmBitsPixel + 15) / 16) * 2;
		const size_t size = stride * bitmap.bmHeight;
		if (size > kMaxResponse / 2)
			return false;
		bitmap.bmWidthBytes = static_cast<LONG>(stride);
		bytes.resize(size);
		return GetBitmapBits(handle, static_cast<LONG>(size), bytes.data()) == static_cast<LONG>(size);
	}
	void append(Response &response) const {
		response.number(bitmap.bmWidth);
		response.number(bitmap.bmHeight);
		response.number(bitmap.bmWidthBytes);
		response.number(bitmap.bmPlanes);
		response.number(bitmap.bmBitsPixel);
		response.bytes(bytes.data(), bytes.size());
	}
};

bool imageResource(const WCHAR *image, const WCHAR *kind, const WCHAR *name, bool icon) {
	LPCWSTR resource = name;
	if (wcscmp(kind, L"id") == 0) {
		WCHAR *end = nullptr;
		const unsigned long id = wcstoul(name, &end, 10);
		if (!*name || *end || id > 65535)
			return false;
		resource = MAKEINTRESOURCEW(id);
	} else if (wcscmp(kind, L"name") != 0)
		return false;
	HMODULE module = nullptr;
	DWORD status = ERROR_SUCCESS;
	if (*image) {
		module = LoadLibraryExW(image, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		if (!module)
			status = GetLastError();
	}
	HICON cursor = nullptr;
	ICONINFO info{};
	BitmapData mask, color;
	if (status == ERROR_SUCCESS) {
		cursor = icon ? LoadIconW(module, resource) : LoadCursorW(module, resource);
		if (!cursor)
			status = GetLastError();
		if (!cursor && status == ERROR_SUCCESS)
			status = ERROR_RESOURCE_NAME_NOT_FOUND;
	}
	if (status == ERROR_SUCCESS) {
		if (!GetIconInfo(cursor, &info))
			status = GetLastError();
		else if (bool(info.fIcon) != icon || !mask.read(info.hbmMask) || (info.hbmColor && !color.read(info.hbmColor)))
			status = ERROR_INVALID_DATA;
	}
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (module)
		FreeLibrary(module);
	Response response;
	response.header(status);
	if (status == ERROR_SUCCESS) {
		response.number(info.fIcon ? 1 : 0);
		response.number(info.xHotspot);
		response.number(info.yHotspot);
		mask.append(response);
		response.number(info.hbmColor ? 1 : 0);
		if (info.hbmColor)
			color.append(response);
	}
	return response.write();
}

bool memoryStatus() {
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	const DWORD error = GlobalMemoryStatusEx(&status) ? ERROR_SUCCESS : GetLastError();
	Response response;
	response.header(error);
	if (error == ERROR_SUCCESS)
		response.bytes(&status, sizeof(status));
	return response.write();
}

bool systemMetrics(const WCHAR *indexText, const WCHAR *errorText) {
	WCHAR *end = nullptr;
	const auto index = wcstoll(indexText, &end, 10);
	if (!*indexText || *end || index < INT32_MIN || index > INT32_MAX)
		return false;
	const auto error = wcstoull(errorText, &end, 10);
	if (!*errorText || *end || error > UINT32_MAX)
		return false;
	SetLastError(static_cast<DWORD>(error));
	const int value = GetSystemMetrics(static_cast<int>(index));
	const DWORD lastError = GetLastError();
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(value));
	response.number(lastError);
	return response.write();
}

bool registry(const WCHAR *pathText, const WCHAR *name, const WCHAR *view, bool snapshot = false) {
	std::wstring path(pathText);
	const auto separator = path.find(L'\\');
	const auto rootName = path.substr(0, separator);
	const WCHAR *subkey = separator == std::wstring::npos ? L"" : path.c_str() + separator + 1;
	HKEY root = nullptr;
	if (_wcsicmp(rootName.c_str(), L"HKEY_LOCAL_MACHINE") == 0)
		root = HKEY_LOCAL_MACHINE;
	else if (_wcsicmp(rootName.c_str(), L"HKEY_CURRENT_USER") == 0)
		root = HKEY_CURRENT_USER;
	else if (_wcsicmp(rootName.c_str(), L"HKEY_CLASSES_ROOT") == 0)
		root = HKEY_CLASSES_ROOT;
	else if (_wcsicmp(rootName.c_str(), L"HKEY_USERS") == 0)
		root = HKEY_USERS;
	else if (_wcsicmp(rootName.c_str(), L"HKEY_CURRENT_CONFIG") == 0)
		root = HKEY_CURRENT_CONFIG;
	REGSAM access = KEY_QUERY_VALUE;
	if (wcscmp(view, L"64") == 0)
		access |= KEY_WOW64_64KEY;
	else if (wcscmp(view, L"32") == 0)
		access |= KEY_WOW64_32KEY;
	else
		return false;
	HKEY key = nullptr;
	LSTATUS status = root ? RegOpenKeyExW(root, subkey, 0, access, &key) : ERROR_INVALID_PARAMETER;
	DWORD type = 0, size = 0;
	std::vector<BYTE> value;
	if (status == ERROR_SUCCESS && name) {
		status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
		if (status == ERROR_SUCCESS && size > kMaxResponse - 20)
			status = ERROR_NOT_ENOUGH_MEMORY;
		if (status == ERROR_SUCCESS) {
			value.resize(size);
			status = RegQueryValueExW(key, name, nullptr, &type, value.data(), &size);
			if (status == ERROR_SUCCESS)
				value.resize(size);
		}
	}
	struct Value {
		std::vector<WCHAR> name;
		DWORD type;
		std::vector<BYTE> bytes;
	};
	std::vector<Value> values;
	if (snapshot && status == ERROR_SUCCESS) {
		size_t total = 16;
		for (DWORD index = 0;; ++index) {
			Value entry{{}, 0, {}};
			entry.name.resize(16384);
			DWORD characters = static_cast<DWORD>(entry.name.size()), bytes = 0;
			status = RegEnumValueW(key, index, entry.name.data(), &characters, nullptr, &entry.type, nullptr, &bytes);
			if (status == ERROR_NO_MORE_ITEMS) {
				status = ERROR_SUCCESS;
				break;
			}
			if (status != ERROR_SUCCESS)
				break;
			if (total + bytes + characters * sizeof(WCHAR) + 12 > kMaxResponse || index >= 4096) {
				status = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}
			entry.bytes.resize(bytes);
			characters = static_cast<DWORD>(entry.name.size());
			status = RegEnumValueW(key, index, entry.name.data(), &characters, nullptr, &entry.type, entry.bytes.data(),
								   &bytes);
			if (status != ERROR_SUCCESS)
				break;
			entry.name.resize(characters);
			entry.bytes.resize(bytes);
			total += 12 + bytes + characters * sizeof(WCHAR);
			values.push_back(std::move(entry));
		}
	}
	if (key)
		RegCloseKey(key);
	Response response;
	response.header(status);
	if (snapshot && status == ERROR_SUCCESS) {
		response.number(static_cast<uint32_t>(values.size()));
		for (const auto &entry : values) {
			response.bytes(entry.name.data(), entry.name.size() * sizeof(WCHAR));
			response.number(entry.type);
			response.bytes(entry.bytes.data(), entry.bytes.size());
		}
	} else if (name && status == ERROR_SUCCESS) {
		response.number(type);
		response.bytes(value.data(), value.size());
	}
	return response.write();
}
} // namespace

int wmain(int argc, WCHAR **argv) {
	_setmode(_fileno(stdout), _O_BINARY);
	bool written = false;
	if ((argc == 3 || argc == 8) && wcscmp(argv[1], L"management-connect") == 0)
		written = management(argv[2], nullptr, argc == 8 ? argv + 3 : nullptr);
	else if ((argc == 4 || argc == 9) && wcscmp(argv[1], L"management-query") == 0)
		written = management(argv[2], argv[3], argc == 9 ? argv + 4 : nullptr);
	else if (argc == 5 && wcscmp(argv[1], L"set-file-security-a") == 0)
		written = setFileSecurity(argv[2], argv[3], argv[4], true);
	else if (argc == 5 && wcscmp(argv[1], L"set-file-security-w") == 0)
		written = setFileSecurity(argv[2], argv[3], argv[4], false);
	else if (argc == 4 && wcscmp(argv[1], L"file-security-a") == 0)
		written = fileSecurity(argv[2], argv[3], true);
	else if (argc == 4 && wcscmp(argv[1], L"file-security-w") == 0)
		written = fileSecurity(argv[2], argv[3], false);
	else if (argc == 6 && wcscmp(argv[1], L"image-load") == 0 &&
			 (wcscmp(argv[2], L"cursor") == 0 || wcscmp(argv[2], L"icon") == 0))
		written = imageResource(argv[3], argv[4], argv[5], wcscmp(argv[2], L"icon") == 0);
	else if (argc == 6 && wcscmp(argv[1], L"format-message") == 0 &&
			 (wcscmp(argv[2], L"a") == 0 || wcscmp(argv[2], L"w") == 0))
		written = formatMessage(argv + 3, wcscmp(argv[2], L"w") == 0);
	else if (argc == 3 && wcscmp(argv[1], L"network-connectivity") == 0)
		written = networkConnectivity(argv[2]);
	else if (argc == 3 && wcscmp(argv[1], L"ip-address-table") == 0 &&
			 (wcscmp(argv[2], L"0") == 0 || wcscmp(argv[2], L"1") == 0))
		written = ipAddressTable(wcscmp(argv[2], L"1") == 0);
	else if (argc == 2 && wcscmp(argv[1], L"memory-status") == 0)
		written = memoryStatus();
	else if (argc == 4 && wcscmp(argv[1], L"system-metrics") == 0)
		written = systemMetrics(argv[2], argv[3]);
	else if (argc == 2 && wcscmp(argv[1], L"user-name") == 0)
		written = userName();
	else if (argc == 4 && wcscmp(argv[1], L"account-lookup-a") == 0)
		written = account(argv[2], argv[3], true);
	else if (argc == 4 && wcscmp(argv[1], L"account-lookup-w") == 0)
		written = account(argv[2], argv[3], false);
	else if (argc == 4 && wcscmp(argv[1], L"registry-snapshot") == 0)
		written = registry(argv[2], nullptr, argv[3], true);
	else if (argc == 4 && wcscmp(argv[1], L"registry-open") == 0)
		written = registry(argv[2], nullptr, argv[3]);
	else if (argc == 5 && wcscmp(argv[1], L"registry-query") == 0)
		written = registry(argv[2], argv[3], argv[4]);
	return written ? 0 : 1;
}
