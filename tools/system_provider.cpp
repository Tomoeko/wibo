// Standalone Windows service adapter. Build with a Windows C++ compiler and
// link ole32, oleaut32, and wbemuuid. The caller selects its execution environment.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>

#include <fcntl.h>
#include <io.h>
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

bool registry(const WCHAR *pathText, const WCHAR *name, const WCHAR *view) {
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
	if (key)
		RegCloseKey(key);
	Response response;
	response.header(status);
	if (name && status == ERROR_SUCCESS) {
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
	else if (argc == 4 && wcscmp(argv[1], L"registry-open") == 0)
		written = registry(argv[2], nullptr, argv[3]);
	else if (argc == 5 && wcscmp(argv[1], L"registry-query") == 0)
		written = registry(argv[2], argv[3], argv[4]);
	return written ? 0 : 1;
}
