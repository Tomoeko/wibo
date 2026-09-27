// Standalone Windows service adapter. The caller selects its execution environment.
// The build target supplies the compiler and system libraries.
#define CINTERFACE
#define COBJMACROS
#include "../src/security_descriptor.h"
#include <winsock2.h>
#include <ws2ipdef.h>

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <oleauto.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wbemcli.h>
#include <winternl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "adapter_addresses.h"

namespace {
constexpr size_t kMaxResponse = 8 * 1024 * 1024;

bool streamResponse = false;

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
	bool write() const {
		if (!valid)
			return false;
		if (streamResponse) {
			const uint32_t size = static_cast<uint32_t>(data.size());
			const BYTE length[4] = {BYTE(size), BYTE(size >> 8), BYTE(size >> 16), BYTE(size >> 24)};
			if (fwrite(length, 1, sizeof(length), stdout) != sizeof(length))
				return false;
		}
		return fwrite(data.data(), 1, data.size(), stdout) == data.size() && fflush(stdout) == 0;
	}
	bool good() const { return valid; }
};

bool apiSetHost(const WCHAR *contract) {
	HMODULE module = LoadLibraryW(contract);
	Response response;
	if (!module) {
		response.header(GetLastError());
		return response.write();
	}
	constexpr DWORD kPathCapacity = 32768;
	WCHAR path[kPathCapacity];
	const DWORD length = GetModuleFileNameW(module, path, kPathCapacity);
	DWORD status = ERROR_SUCCESS;
	if (!length)
		status = GetLastError();
	else if (length >= kPathCapacity)
		status = ERROR_INSUFFICIENT_BUFFER;
	FreeLibrary(module);
	response.header(status);
	if (!status) {
		const WCHAR *base = path;
		for (const WCHAR *cursor = path; *cursor; ++cursor)
			if (*cursor == L'\\' || *cursor == L'/')
				base = cursor + 1;
		response.bytes(base, wcslen(base) * sizeof(WCHAR));
	}
	return response.write();
}

bool numaHighestNodeNumber() {
	ULONG highest = 0;
	const BOOL result = GetNumaHighestNodeNumber(&highest);
	Response response;
	response.header(result ? ERROR_SUCCESS : GetLastError());
	if (result)
		response.number(highest);
	return response.write();
}

bool timeZoneInformation() {
	TIME_ZONE_INFORMATION zone{};
	const DWORD state = GetTimeZoneInformation(&zone);
	Response response;
	response.header(state == TIME_ZONE_ID_INVALID ? GetLastError() : ERROR_SUCCESS);
	if (state != TIME_ZONE_ID_INVALID) {
		response.number(state);
		response.bytes(&zone, sizeof(zone));
	}
	return response.write();
}

bool dynamicTimeZoneInformation() {
	DYNAMIC_TIME_ZONE_INFORMATION zone{};
	const DWORD state = GetDynamicTimeZoneInformation(&zone);
	Response response;
	response.header(state == TIME_ZONE_ID_INVALID ? GetLastError() : ERROR_SUCCESS);
	if (state != TIME_ZONE_ID_INVALID) {
		response.number(state);
		response.bytes(&zone, sizeof(zone));
	}
	return response.write();
}

bool environmentDefaults() {
	constexpr const char *names[] = {"APPDATA",		 "LOCALAPPDATA", "ALLUSERSPROFILE", "PROGRAMDATA",
									 "SYSTEMROOT",	 "WINDIR",		 "USERPROFILE",		"HOMEDRIVE",
									 "HOMEPATH",	 "PUBLIC",		 "PROGRAMFILES",	"PROGRAMFILES(X86)",
									 "PROGRAMW6432", "COMSPEC",		 "SYSTEMDRIVE"};
	std::vector<std::pair<const char *, std::string>> values;
	for (const char *name : names) {
		DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
		if (!size)
			continue;
		if (size > 32768)
			return false;
		std::vector<char> value(size);
		const DWORD length = GetEnvironmentVariableA(name, value.data(), size);
		if (length >= size)
			return false;
		values.emplace_back(name, std::string(value.data(), length));
	}
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(values.size()));
	for (const auto &[name, value] : values) {
		response.bytes(name, strlen(name));
		response.bytes(value.data(), value.size());
	}
	return response.write();
}

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

bool parseUnsignedDecimal(const WCHAR *text, uint32_t maximum, uint32_t &value) {
	if (!*text)
		return false;
	value = 0;
	for (; *text; ++text) {
		if (*text < L'0' || *text > L'9')
			return false;
		const uint32_t digit = *text - L'0';
		if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
			return false;
		value = value * 10 + digit;
	}
	return true;
}

bool alphabeticCharacterTable() {
	static_assert(sizeof(WCHAR) == 2);
	BYTE table[0x10000 / 8]{};
	for (uint32_t code = 0; code < 0x10000; ++code) {
		SetLastError(ERROR_SUCCESS);
		const BOOL alphabetic = IsCharAlphaW(static_cast<WCHAR>(code));
		if (GetLastError() != ERROR_SUCCESS) {
			// A classification table cannot represent character-specific API errors.
			Response response;
			response.header(ERROR_NOT_SUPPORTED);
			return response.write();
		}
		if (alphabetic)
			table[code / 8] |= static_cast<BYTE>(1u << (code % 8));
	}
	Response response;
	response.header(ERROR_SUCCESS);
	response.bytes(table, sizeof(table));
	return response.write();
}

bool parseMappingCount(const WCHAR *text, int &value) {
	const bool negative = *text == L'-';
	uint32_t magnitude;
	if (!parseUnsignedDecimal(text + negative, negative ? uint32_t(INT32_MAX) + 1 : INT32_MAX, magnitude))
		return false;
	value = static_cast<int>(negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude));
	return true;
}

bool decodeMappingString(const WCHAR *text, std::vector<WCHAR> &value) {
	static_assert(sizeof(WCHAR) == 2);
	const size_t length = wcslen(text);
	if (length % 4 || length / 2 > kMaxResponse)
		return false;
	std::string bytes;
	if (!decodeHex(text, bytes, true))
		return false;
	value.resize(bytes.size() / sizeof(WCHAR));
	if (!bytes.empty())
		std::memcpy(value.data(), bytes.data(), bytes.size());
	return true;
}

bool validCountedString(const std::vector<WCHAR> &value, int count) {
	if (count >= 0)
		return value.size() == static_cast<size_t>(count);
	if (value.empty() || value.back())
		return false;
	for (size_t index = 0; index + 1 < value.size(); ++index)
		if (!value[index])
			return false;
	return true;
}

bool isValidLocaleName(const WCHAR *nameText, const WCHAR *errorText) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t incomingError = 0;
	std::vector<WCHAR> name;
	if (!parseUnsignedDecimal(errorText, UINT32_MAX, incomingError) || !decodeMappingString(nameText, name) ||
		name.size() >= LOCALE_NAME_MAX_LENGTH || std::find(name.begin(), name.end(), WCHAR{0}) != name.end())
		return fail(ERROR_INVALID_PARAMETER);
	name.push_back(0);
	SetLastError(incomingError);
	const BOOL result = IsValidLocaleName(name.data());
	const DWORD nativeError = GetLastError();
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(result ? 1 : 0);
	response.number(nativeError);
	return response.write();
}

bool stringTypeExA(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	constexpr size_t kMaxSourceBytes = (64 * 1024 - 256) / 6;
	uint32_t locale = 0, type = 0, incomingError = 0;
	int count = 0;
	std::string source, initial;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, locale) ||
		!parseUnsignedDecimal(parameters[1], UINT32_MAX, type) || !parseMappingCount(parameters[2], count) ||
		!parseUnsignedDecimal(parameters[5], UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	if (count < -1)
		return fail(ERROR_NOT_SUPPORTED);
	if (wcslen(parameters[3]) > kMaxSourceBytes * 2 || !decodeHex(parameters[3], source, true) ||
		(count >= 0 ? source.size() != static_cast<size_t>(count)
					: source.empty() || source.back() || source.find('\0') != source.size() - 1))
		return fail(ERROR_INVALID_PARAMETER);
	const size_t outputBytes = source.size() * sizeof(WORD);
	if (wcslen(parameters[4]) != outputBytes * 2 || !decodeHex(parameters[4], initial, true) ||
		initial.size() != outputBytes)
		return fail(ERROR_INVALID_PARAMETER);
	constexpr size_t kGuardWords = 8;
	constexpr WORD kGuardValue = 0xa5a5;
	std::vector<WORD> output(source.size() + kGuardWords, kGuardValue);
	if (outputBytes)
		std::memcpy(output.data(), initial.data(), outputBytes);
	// Even an empty source retains a valid pointer for the genuine zero-count call.
	source.push_back(0);
	SetLastError(incomingError);
	const BOOL result = GetStringTypeExA(locale, type, source.data(), count, output.data());
	const DWORD nativeError = GetLastError();
	if (!std::all_of(output.begin() + outputBytes / sizeof(WORD), output.end(),
					 [](WORD value) { return value == kGuardValue; }))
		return fail(ERROR_NOT_SUPPORTED);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(result ? 1 : 0);
	response.number(nativeError);
	response.bytes(output.data(), outputBytes);
	return response.write();
}

bool upperCharacterBuffer(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t count = 0, incomingError = 0;
	std::vector<WCHAR> buffer;
	if (!parseUnsignedDecimal(parameters[0], 16000, count) || !count ||
		!parseUnsignedDecimal(parameters[2], UINT32_MAX, incomingError) ||
		!decodeMappingString(parameters[1], buffer) || buffer.size() != count)
		return fail(ERROR_INVALID_PARAMETER);
	SetLastError(incomingError);
	const DWORD result = CharUpperBuffW(buffer.data(), count);
	const DWORD nativeError = GetLastError();
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(result);
	response.number(nativeError);
	response.bytes(buffer.data(), buffer.size() * sizeof(WCHAR));
	return response.write();
}

bool lcMapStringEx(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t flags;
	int sourceCount, destinationCapacity;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, flags) || !parseMappingCount(parameters[2], sourceCount) ||
		!parseMappingCount(parameters[4], destinationCapacity))
		return fail(ERROR_INVALID_PARAMETER);
	unsigned destinationMode;
	if (wcscmp(parameters[5], L"0") == 0)
		destinationMode = 0;
	else if (wcscmp(parameters[5], L"1") == 0)
		destinationMode = 1;
	else if (wcscmp(parameters[5], L"2") == 0)
		destinationMode = 2;
	else
		return fail(ERROR_INVALID_PARAMETER);
	if (destinationCapacity < 0)
		return fail(ERROR_INVALID_PARAMETER);
	if (!destinationMode && destinationCapacity > 0)
		return fail(ERROR_INSUFFICIENT_BUFFER);
	if (flags & (LCMAP_HASH | LCMAP_SORTHANDLE))
		return fail(ERROR_NOT_SUPPORTED);

	const bool hasLocale = wcscmp(parameters[1], L"-") != 0;
	std::vector<WCHAR> locale, source;
	if ((hasLocale && !decodeMappingString(parameters[1], locale)) || !decodeMappingString(parameters[3], source))
		return fail(ERROR_INVALID_PARAMETER);
	for (WCHAR character : locale)
		if (!character)
			return fail(ERROR_INVALID_PARAMETER);
	if (!validCountedString(source, sourceCount))
		return fail(ERROR_INVALID_PARAMETER);

	const bool sortKey = flags & LCMAP_SORTKEY;
	const size_t outputUnit = sortKey ? 1 : sizeof(WCHAR);
	constexpr size_t kResponseOverhead = 5 * sizeof(uint32_t);
	if (static_cast<size_t>(destinationCapacity) > (kMaxResponse - kResponseOverhead) / outputUnit)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	const size_t destinationBytes = static_cast<size_t>(destinationCapacity) * outputUnit;
	const size_t destinationUnits = (destinationBytes + sizeof(WCHAR) - 1) / sizeof(WCHAR);
	const size_t sourceUnits =
		destinationMode == 2 && destinationUnits > source.size() ? destinationUnits : source.size();
	const size_t separateUnits = destinationMode == 1 ? destinationUnits : 0;
	if (sourceUnits + separateUnits + locale.size() + hasLocale > kMaxResponse / sizeof(WCHAR))
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	if (hasLocale)
		locale.push_back(0);
	if (destinationMode == 2)
		source.resize(sourceUnits);
	std::vector<WCHAR> separateDestination(separateUnits);
	WCHAR *destination = nullptr;
	if (destinationCapacity > 0)
		destination = destinationMode == 2 ? source.data() : separateDestination.data();
	SetLastError(ERROR_SUCCESS);
	const int result =
		LCMapStringEx(hasLocale ? locale.data() : nullptr, flags, source.empty() ? nullptr : source.data(), sourceCount,
					  destination, destinationCapacity, nullptr, nullptr, 0);
	if (!result) {
		const DWORD error = GetLastError();
		return fail(error ? error : ERROR_INVALID_DATA);
	}
	if (result < 0 || (destinationCapacity && result > destinationCapacity))
		return fail(ERROR_INVALID_DATA);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(result));
	response.bytes(destination, destinationCapacity ? static_cast<size_t>(result) * outputUnit : 0);
	return response.write();
}

bool compareStringEx(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t flags;
	int leftCount, rightCount;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, flags) || !parseMappingCount(parameters[2], leftCount) ||
		!parseMappingCount(parameters[4], rightCount))
		return fail(ERROR_INVALID_PARAMETER);
	const bool hasLocale = wcscmp(parameters[1], L"-") != 0;
	size_t totalUnits = hasLocale;
	const WCHAR *encodedStrings[] = {hasLocale ? parameters[1] : L"", parameters[3], parameters[5]};
	for (unsigned index = 0; index < 3; ++index) {
		const size_t length = wcslen(encodedStrings[index]);
		if (length % 4)
			return fail(ERROR_INVALID_PARAMETER);
		const size_t units = index != 0 && length == 0 ? 1 : length / 4;
		if (units > kMaxResponse / sizeof(WCHAR) - totalUnits)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		totalUnits += units;
	}
	std::vector<WCHAR> locale, left, right;
	if ((hasLocale && !decodeMappingString(parameters[1], locale)) || !decodeMappingString(parameters[3], left) ||
		!decodeMappingString(parameters[5], right) || !validCountedString(left, leftCount) ||
		!validCountedString(right, rightCount))
		return fail(ERROR_INVALID_PARAMETER);
	for (WCHAR character : locale)
		if (!character)
			return fail(ERROR_INVALID_PARAMETER);
	const size_t leftUnits = left.empty() ? 1 : left.size();
	const size_t rightUnits = right.empty() ? 1 : right.size();
	if (hasLocale)
		locale.push_back(0);
	// A zero count still requires a readable, non-null string pointer.
	left.resize(leftUnits);
	right.resize(rightUnits);
	SetLastError(ERROR_SUCCESS);
	const int result = CompareStringEx(hasLocale ? locale.data() : nullptr, flags, left.data(), leftCount, right.data(),
									   rightCount, nullptr, nullptr, 0);
	if (!result) {
		const DWORD error = GetLastError();
		return fail(error ? error : ERROR_INVALID_DATA);
	}
	if (result < CSTR_LESS_THAN || result > CSTR_GREATER_THAN)
		return fail(ERROR_INVALID_DATA);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(result));
	return response.write();
}

bool findNlsStringEx(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t flags, foundRequested, incomingError;
	int sourceCount, valueCount;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, flags) || !parseMappingCount(parameters[2], sourceCount) ||
		!parseMappingCount(parameters[4], valueCount) || !parseUnsignedDecimal(parameters[6], 1, foundRequested) ||
		!parseUnsignedDecimal(parameters[7], UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	constexpr DWORD kFindModes = FIND_FROMSTART | FIND_FROMEND | FIND_STARTSWITH | FIND_ENDSWITH;
	constexpr DWORD kFindFilters = NORM_IGNORECASE | NORM_LINGUISTIC_CASING;
	const DWORD mode = flags & kFindModes;
	if (!mode || (mode & (mode - 1)) || (flags & ~(kFindModes | kFindFilters)))
		return fail(ERROR_NOT_SUPPORTED);
	if (!sourceCount || sourceCount < -1 || !valueCount || valueCount < -1)
		return fail(ERROR_INVALID_PARAMETER);
	const bool hasLocale = wcscmp(parameters[1], L"-") != 0;
	size_t totalUnits = hasLocale;
	const WCHAR *encodedStrings[] = {hasLocale ? parameters[1] : L"", parameters[3], parameters[5]};
	for (const WCHAR *text : encodedStrings) {
		const size_t length = wcslen(text);
		if (length % 4)
			return fail(ERROR_INVALID_PARAMETER);
		if (length / 4 > kMaxResponse / sizeof(WCHAR) - totalUnits)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		totalUnits += length / 4;
	}
	std::vector<WCHAR> locale, source, value;
	if ((hasLocale && !decodeMappingString(parameters[1], locale)) || !decodeMappingString(parameters[3], source) ||
		!decodeMappingString(parameters[5], value) || !validCountedString(source, sourceCount) ||
		!validCountedString(value, valueCount) || locale.size() >= LOCALE_NAME_MAX_LENGTH)
		return fail(ERROR_INVALID_PARAMETER);
	for (WCHAR character : locale)
		if (!character)
			return fail(ERROR_INVALID_PARAMETER);
	if (hasLocale)
		locale.push_back(0);
	const size_t sourceLength = sourceCount == -1 ? source.size() - 1 : source.size();
	// The sentinel cannot be a valid length in the bounded native input buffers.
	int found = INT32_MIN;
	SetLastError(incomingError);
	const int index = FindNLSStringEx(hasLocale ? locale.data() : nullptr, flags, source.data(), sourceCount,
									  value.data(), valueCount, foundRequested ? &found : nullptr, nullptr, nullptr, 0);
	const DWORD nativeError = GetLastError();
	const bool foundPresent = found != INT32_MIN;
	if (index < -1 || (index >= 0 && static_cast<size_t>(index) > sourceLength) ||
		(foundPresent && (!foundRequested || index < 0 || found < 0 ||
						  static_cast<size_t>(found) > sourceLength - static_cast<size_t>(index))) ||
		(foundRequested && index >= 0 && !foundPresent))
		return fail(ERROR_INVALID_DATA);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(index));
	response.number(nativeError);
	response.number(foundPresent);
	response.number(foundPresent ? static_cast<uint32_t>(found) : 0);
	return response.write();
}

bool cpInfoExW(const WCHAR *codePageText, const WCHAR *flagsText) {
	static_assert(sizeof(CPINFOEXW) == 544 && alignof(CPINFOEXW) == 4);
	static_assert(offsetof(CPINFOEXW, MaxCharSize) == 0 && offsetof(CPINFOEXW, DefaultChar) == 4 &&
				  offsetof(CPINFOEXW, LeadByte) == 6 && offsetof(CPINFOEXW, UnicodeDefaultChar) == 18 &&
				  offsetof(CPINFOEXW, CodePage) == 20 && offsetof(CPINFOEXW, CodePageName) == 24);
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		response.number(0);
		return response.write();
	};
	uint32_t codePage, flags;
	if (!parseUnsignedDecimal(codePageText, UINT32_MAX, codePage) ||
		!parseUnsignedDecimal(flagsText, UINT32_MAX, flags) || flags)
		return fail(ERROR_INVALID_PARAMETER);
	CPINFOEXW information{};
	static_assert(sizeof(information.DefaultChar) == 2 && sizeof(information.LeadByte) == 12 &&
				  sizeof(information.CodePageName) == 520);
	SetLastError(ERROR_SUCCESS);
	const BOOL succeeded = GetCPInfoExW(codePage, flags, &information);
	const DWORD status = succeeded ? ERROR_SUCCESS : GetLastError();
	Response response;
	response.header(status);
	response.number(succeeded ? 1 : 0);
	if (succeeded)
		response.bytes(&information, sizeof(information));
	return response.write();
}

enum class LocaleSnapshotOperation { Information, ResolveName };

bool localeBufferSnapshot(WCHAR **parameters, LocaleSnapshotOperation operation, LCTYPE type = 0) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t incomingError = 0;
	int capacity = 0;
	if (!parseMappingCount(parameters[1], capacity) || !parseUnsignedDecimal(parameters[3], UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	if (operation == LocaleSnapshotOperation::ResolveName && capacity < 0)
		return fail(ERROR_NOT_SUPPORTED);
	constexpr size_t kRequestLimit = 64 * 1024;
	constexpr size_t kRequestOverhead = 256;
	constexpr size_t kMaxLocaleHex = 84 * sizeof(WCHAR) * 2;
	const size_t nameLength = wcslen(parameters[0]);
	const size_t seedLength = wcslen(parameters[2]);
	if (nameLength > kMaxLocaleHex)
		return fail(ERROR_INVALID_PARAMETER);
	if (seedLength > kRequestLimit - kRequestOverhead - nameLength)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	const bool hasLocale = wcscmp(parameters[0], L"-") != 0;
	std::vector<WCHAR> locale;
	if (hasLocale) {
		if (!decodeMappingString(parameters[0], locale))
			return fail(ERROR_INVALID_PARAMETER);
		for (WCHAR value : locale)
			if (!value)
				return fail(ERROR_INVALID_PARAMETER);
		locale.push_back(0);
	}
	const bool hasData = wcscmp(parameters[2], L"-") != 0;
	if (!hasData && capacity > 0)
		return fail(ERROR_NOT_SUPPORTED);
	const size_t units = hasData && capacity > 0 ? static_cast<size_t>(capacity) : 0;
	const size_t byteCapacity = units * sizeof(WCHAR);
	constexpr size_t kGuardUnits = 8;
	constexpr WCHAR kGuardValue = 0xa5a5;
	std::vector<WCHAR> buffer;
	if (hasData) {
		if (units > (kRequestLimit - kRequestOverhead - nameLength) / (2 * sizeof(WCHAR)))
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		std::string initial;
		if (seedLength != byteCapacity * 2 || !decodeHex(parameters[2], initial, true) ||
			initial.size() != byteCapacity)
			return fail(ERROR_INVALID_PARAMETER);
		// Retain a non-NULL pointer for zero and negative original capacities.
		buffer.assign(units + kGuardUnits, kGuardValue);
		if (byteCapacity)
			std::memcpy(buffer.data(), initial.data(), byteCapacity);
	}
	SetLastError(incomingError);
	const int result =
		operation == LocaleSnapshotOperation::ResolveName
			? ResolveLocaleName(hasLocale ? locale.data() : nullptr, hasData ? buffer.data() : nullptr, capacity)
			: GetLocaleInfoEx(hasLocale ? locale.data() : nullptr, type, hasData ? buffer.data() : nullptr, capacity);
	const DWORD nativeError = GetLastError();
	if (result < 0 || (result && (capacity < 0 || (capacity > 0 && result > capacity))) ||
		(hasData &&
		 !std::all_of(buffer.begin() + units, buffer.end(), [](WCHAR value) { return value == kGuardValue; })))
		return fail(ERROR_NOT_SUPPORTED);
	if (operation == LocaleSnapshotOperation::ResolveName && result) {
		if (result > LOCALE_NAME_MAX_LENGTH)
			return fail(ERROR_NOT_SUPPORTED);
		if (hasData && capacity > 0)
			for (int index = 0; index < result; ++index)
				if ((buffer[index] == 0) != (index + 1 == result))
					return fail(ERROR_NOT_SUPPORTED);
	}
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(result));
	response.number(nativeError);
	response.bytes(hasData ? buffer.data() : nullptr, byteCapacity);
	return response.write();
}

bool localeInfoEx(WCHAR **parameters) {
	uint32_t type = 0;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, type)) {
		Response response;
		response.header(ERROR_INVALID_PARAMETER);
		return response.write();
	}
	return localeBufferSnapshot(parameters + 1, LocaleSnapshotOperation::Information, type);
}

struct MuiBufferSnapshot {
	static constexpr size_t kGuardUnits = 8;
	static constexpr WCHAR kGuardValue = 0xa5a5;
	bool present = false;
	ULONG capacity = 0;
	std::string initial;
	std::vector<WCHAR> buffer;

	DWORD capture(const WCHAR *seed, ULONG units, bool language) {
		present = wcscmp(seed, L"-") != 0;
		capacity = units;
		if ((!present && capacity) || (language && present && !capacity))
			return ERROR_NOT_SUPPORTED;
		if (!present)
			return ERROR_SUCCESS;
		const size_t bytes = static_cast<size_t>(capacity) * sizeof(WCHAR);
		if (wcslen(seed) != bytes * 2 || !decodeHex(seed, initial, true) || initial.size() != bytes)
			return ERROR_INVALID_PARAMETER;
		buffer.assign(static_cast<size_t>(capacity) + kGuardUnits, kGuardValue);
		if (bytes)
			std::memcpy(buffer.data(), initial.data(), bytes);
		if (language && std::find(buffer.begin(), buffer.begin() + capacity, WCHAR(0)) == buffer.begin() + capacity)
			return ERROR_NOT_SUPPORTED;
		return ERROR_SUCCESS;
	}

	WCHAR *data() { return present ? buffer.data() : nullptr; }

	[[nodiscard]] bool unchanged() const {
		return !present ||
			   ((initial.empty() || std::memcmp(buffer.data(), initial.data(), initial.size()) == 0) &&
				std::all_of(buffer.begin() + capacity, buffer.end(), [](WCHAR value) { return value == kGuardValue; }));
	}
};

bool fileMuiPath(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	static_assert(sizeof(ULONG) == 4 && sizeof(ULONGLONG) == 8 && sizeof(WCHAR) == 2);
	uint32_t flags = 0, languageCapacity = 0, pathCapacity = 0, incomingError = 0;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, flags) ||
		!parseUnsignedDecimal(parameters[2], UINT32_MAX, languageCapacity) ||
		!parseUnsignedDecimal(parameters[4], UINT32_MAX, pathCapacity) ||
		!parseUnsignedDecimal(parameters[7], UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	constexpr size_t kRequestLimit = 64 * 1024;
	constexpr size_t kRequestOverhead = 256;
	const uint64_t bufferHexSize = (uint64_t(languageCapacity) + pathCapacity) * 2 * sizeof(WCHAR);
	if (bufferHexSize >= kRequestLimit - kRequestOverhead)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	size_t remaining = kRequestLimit - kRequestOverhead;
	for (size_t index : {size_t(1), size_t(3), size_t(5)}) {
		const size_t length = wcslen(parameters[index]);
		if (length > remaining)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		remaining -= length;
	}
	if (wcscmp(parameters[1], L"-") == 0)
		return fail(ERROR_NOT_SUPPORTED);
	std::vector<WCHAR> filename;
	if (!decodeMappingString(parameters[1], filename) ||
		std::find(filename.begin(), filename.end(), WCHAR(0)) != filename.end())
		return fail(ERROR_INVALID_PARAMETER);
	filename.push_back(0);
	MuiBufferSnapshot language, path;
	DWORD status = language.capture(parameters[3], languageCapacity, true);
	if (status != ERROR_SUCCESS)
		return fail(status);
	status = path.capture(parameters[5], pathCapacity, false);
	if (status != ERROR_SUCCESS)
		return fail(status);
	std::string enumeratorBytes;
	if (wcslen(parameters[6]) != sizeof(ULONGLONG) * 2 || !decodeHex(parameters[6], enumeratorBytes, true) ||
		enumeratorBytes.size() != sizeof(ULONGLONG))
		return fail(ERROR_INVALID_PARAMETER);
	ULONGLONG enumerator = 0;
	std::memcpy(&enumerator, enumeratorBytes.data(), sizeof(enumerator));
	ULONG languageCount = languageCapacity, pathCount = pathCapacity;
	SetLastError(incomingError);
	const BOOL result =
		GetFileMUIPath(flags, filename.data(), language.data(), &languageCount, path.data(), &pathCount, &enumerator);
	const DWORD nativeError = GetLastError();
	// Only forward a native unavailable-service result with unchanged snapshots.
	// Successful resource paths and opaque enumeration state need separate ownership.
	if (result || nativeError != ERROR_CALL_NOT_IMPLEMENTED || languageCount != languageCapacity ||
		pathCount != pathCapacity || std::memcmp(&enumerator, enumeratorBytes.data(), sizeof(enumerator)) != 0 ||
		!language.unchanged() || !path.unchanged())
		return fail(ERROR_NOT_SUPPORTED);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(0);
	response.number(nativeError);
	response.number(languageCount);
	response.number(pathCount);
	response.bytes(&enumerator, sizeof(enumerator));
	response.bytes(language.data(), language.initial.size());
	response.bytes(path.data(), path.initial.size());
	return response.write();
}

bool userPreferredUiLanguages(const WCHAR *flagsText, const WCHAR *capacityText, const WCHAR *modeText) {
	uint32_t flags = 0, capacity = 0, mode = 0;
	const auto write = [](DWORD status, BOOL result, bool countPresent, ULONG count, ULONG units,
						  const std::vector<WCHAR> &languages) {
		Response response;
		response.header(status);
		response.number(result ? 1 : 0);
		response.number(countPresent ? 1 : 0);
		response.number(countPresent ? count : 0);
		response.number(units);
		if (result)
			response.bytes(languages.data(), languages.size() * sizeof(WCHAR));
		return response.write();
	};
	if (!parseUnsignedDecimal(flagsText, UINT32_MAX, flags) ||
		!parseUnsignedDecimal(capacityText, UINT32_MAX, capacity) || !parseUnsignedDecimal(modeText, 1, mode) ||
		(flags & ~(MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME)) ||
		(flags & (MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME)) == (MUI_LANGUAGE_ID | MUI_LANGUAGE_NAME))
		return write(ERROR_INVALID_PARAMETER, FALSE, false, 0, capacity, {});
	constexpr size_t kMaxLanguageUnits = (kMaxResponse - 32) / sizeof(WCHAR);
	if (mode && capacity > kMaxLanguageUnits)
		return write(ERROR_NOT_ENOUGH_MEMORY, FALSE, false, 0, capacity, {});
	std::vector<WCHAR> languages;
	if (mode)
		languages.resize(capacity ? capacity : 1);
	// A valid list cannot contain ULONG_MAX entries within a ULONG character count.
	constexpr ULONG kUnwrittenCount = UINT32_MAX;
	ULONG count = kUnwrittenCount, units = capacity;
	SetLastError(ERROR_SUCCESS);
	const BOOL result = GetUserPreferredUILanguages(flags, &count, mode ? languages.data() : nullptr, &units);
	const DWORD status = result ? ERROR_SUCCESS : GetLastError();
	if (!result && status != ERROR_INSUFFICIENT_BUFFER)
		return write(status, FALSE, false, 0, capacity, {});
	const bool countPresent = count != kUnwrittenCount;
	if (countPresent && (units < 2 || units > kMaxLanguageUnits || count > (units - 1) / 2 || (!count && units != 2) ||
						 ((flags & MUI_LANGUAGE_ID) && count && uint64_t(count) * 5 + 1 != units)))
		return write(ERROR_INVALID_DATA, FALSE, false, 0, capacity, {});
	if (result) {
		const bool filling = mode && capacity;
		if (!countPresent || (!filling && (mode || capacity)) || (filling && units > capacity))
			return write(ERROR_INVALID_DATA, FALSE, false, 0, capacity, {});
		if (mode && capacity)
			languages.resize(units);
		else
			languages.clear();
	} else {
		if (units < 2 || units > kMaxLanguageUnits || (mode && units <= capacity))
			return write(ERROR_INVALID_DATA, FALSE, false, 0, capacity, {});
		languages.clear();
	}
	return write(status, result, countPresent, count, units, languages);
}

class VersionLibrary {
	HMODULE module = nullptr;
	DWORD loadError = ERROR_SUCCESS;

  public:
	VersionLibrary() : module(LoadLibraryW(L"version.dll")) {
		if (!module)
			loadError = GetLastError();
	}
	~VersionLibrary() {
		if (module)
			FreeLibrary(module);
	}
	VersionLibrary(const VersionLibrary &) = delete;
	VersionLibrary &operator=(const VersionLibrary &) = delete;

	template <typename Function> Function resolve(const char *name) {
		if (!module)
			return nullptr;
		const auto address = GetProcAddress(module, name);
		if (!address) {
			loadError = GetLastError();
			return nullptr;
		}
		static_assert(sizeof(Function) == sizeof(address));
		Function function;
		std::memcpy(&function, &address, sizeof(function));
		return function;
	}

	[[nodiscard]] DWORD error() const { return loadError; }
};

bool fileVersionInfoSizeExW(const WCHAR *flagsText, const WCHAR *filenameText, const WCHAR *handleText,
							const WCHAR *lastErrorText) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t flags = 0, hasHandle = 0, incomingError = 0;
	if (!parseUnsignedDecimal(flagsText, UINT32_MAX, flags) || !parseUnsignedDecimal(handleText, 1, hasHandle) ||
		!parseUnsignedDecimal(lastErrorText, UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	const bool hasFilename = wcscmp(filenameText, L"-") != 0;
	std::vector<WCHAR> filename;
	if (hasFilename) {
		if (!decodeMappingString(filenameText, filename))
			return fail(ERROR_INVALID_PARAMETER);
		for (WCHAR value : filename)
			if (!value)
				return fail(ERROR_INVALID_PARAMETER);
		filename.push_back(0);
	}
	using QueryVersionSize = DWORD(WINAPI *)(DWORD, LPCWSTR, LPDWORD);
	VersionLibrary library;
	const auto query = library.resolve<QueryVersionSize>("GetFileVersionInfoSizeExW");
	if (!query)
		return fail(library.error());
	constexpr DWORD kUnwrittenHandle = UINT32_MAX;
	DWORD handle = kUnwrittenHandle;
	SetLastError(incomingError);
	const DWORD size = query(flags, hasFilename ? filename.data() : nullptr, hasHandle ? &handle : nullptr);
	const DWORD nativeError = GetLastError();
	const bool handlePresent = hasHandle && handle != kUnwrittenHandle;
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(size);
	response.number(nativeError);
	response.number(handlePresent ? 1 : 0);
	response.number(handlePresent ? handle : 0);
	return response.write();
}

bool fileVersionInfoExW(WCHAR **parameters) {
	const auto fail = [](DWORD status) {
		Response response;
		response.header(status);
		return response.write();
	};
	uint32_t flags = 0, handle = 0, capacity = 0, incomingError = 0;
	if (!parseUnsignedDecimal(parameters[0], UINT32_MAX, flags) ||
		!parseUnsignedDecimal(parameters[2], UINT32_MAX, handle) ||
		!parseUnsignedDecimal(parameters[3], UINT32_MAX, capacity) ||
		!parseUnsignedDecimal(parameters[5], UINT32_MAX, incomingError))
		return fail(ERROR_INVALID_PARAMETER);
	constexpr size_t kRequestLimit = 64 * 1024;
	constexpr size_t kRequestOverhead = 256;
	const size_t nameLength = wcslen(parameters[1]);
	const size_t seedLength = wcslen(parameters[4]);
	if (nameLength > kRequestLimit - kRequestOverhead || seedLength > kRequestLimit - kRequestOverhead - nameLength)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	const bool hasFilename = wcscmp(parameters[1], L"-") != 0;
	std::vector<WCHAR> filename;
	if (hasFilename) {
		if (!decodeMappingString(parameters[1], filename))
			return fail(ERROR_INVALID_PARAMETER);
		for (WCHAR value : filename)
			if (!value)
				return fail(ERROR_INVALID_PARAMETER);
		filename.push_back(0);
	}
	const bool hasData = wcscmp(parameters[4], L"-") != 0;
	constexpr size_t kGuardBytes = 16;
	constexpr BYTE kGuardValue = 0xa5;
	constexpr size_t kMinVersionBuffer = 92;
	std::string initial;
	std::vector<BYTE> buffer;
	if (hasData) {
		if (capacity < kMinVersionBuffer)
			return fail(ERROR_NOT_SUPPORTED);
		if (capacity > kRequestLimit / 2 || capacity > kMaxResponse - 24)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		if (seedLength != uint64_t(capacity) * 2 || !decodeHex(parameters[4], initial, true) ||
			initial.size() != capacity)
			return fail(ERROR_INVALID_PARAMETER);
		buffer.assign(initial.begin(), initial.end());
		buffer.resize(capacity + kGuardBytes, kGuardValue);
	}
	using QueryVersionInfo = BOOL(WINAPI *)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
	VersionLibrary library;
	const auto query = library.resolve<QueryVersionInfo>("GetFileVersionInfoExW");
	if (!query)
		return fail(library.error());
	SetLastError(incomingError);
	const BOOL result =
		query(flags, hasFilename ? filename.data() : nullptr, handle, capacity, hasData ? buffer.data() : nullptr);
	const DWORD nativeError = GetLastError();
	if ((result && !hasData) || (hasData && !std::all_of(buffer.begin() + capacity, buffer.end(),
														 [](BYTE value) { return value == kGuardValue; })))
		return fail(ERROR_NOT_SUPPORTED);
	Response response;
	response.header(ERROR_SUCCESS);
	response.number(result ? 1 : 0);
	response.number(nativeError);
	response.bytes(hasData ? buffer.data() : nullptr, hasData ? capacity : 0);
	return response.write();
}

bool deviceInfoSetA(const WCHAR *identity, const WCHAR *enumeratorText, const WCHAR *flagsText) {
	std::string classBytes, enumerator;
	GUID classGuid{};
	const bool hasClass = wcscmp(identity, L"-") != 0;
	const bool hasEnumerator = wcscmp(enumeratorText, L"-") != 0;
	if (hasClass) {
		if (!decodeHex(identity, classBytes, true) || classBytes.size() != sizeof(classGuid))
			return false;
		std::memcpy(&classGuid, classBytes.data(), sizeof(classGuid));
	}
	if (hasEnumerator && !decodeHex(enumeratorText, enumerator))
		return false;
	WCHAR *end = nullptr;
	const auto flags = wcstoull(flagsText, &end, 10);
	if (!*flagsText || *end || flags > UINT32_MAX)
		return false;
	const HDEVINFO set =
		SetupDiGetClassDevsA(hasClass ? &classGuid : nullptr, hasEnumerator ? enumerator.c_str() : nullptr, nullptr,
							 static_cast<DWORD>(flags));
	DWORD error = set == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
	struct Entry {
		GUID classGuid;
		DWORD deviceInstance;
	};
	std::vector<Entry> entries;
	if (!error) {
		constexpr size_t kMaxEntries = (kMaxResponse - 4 * sizeof(uint32_t)) / (sizeof(GUID) + 2 * sizeof(uint32_t));
		for (DWORD index = 0;; ++index) {
			SP_DEVINFO_DATA device{};
			device.cbSize = sizeof(device);
			if (!SetupDiEnumDeviceInfo(set, index, &device)) {
				error = GetLastError();
				if (error == ERROR_NO_MORE_ITEMS)
					error = ERROR_SUCCESS;
				break;
			}
			if (entries.size() == kMaxEntries) {
				error = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}
			entries.push_back({device.ClassGuid, device.DevInst});
		}
	}
	if (set != INVALID_HANDLE_VALUE && !SetupDiDestroyDeviceInfoList(set) && !error)
		error = GetLastError();
	Response response;
	response.header(error);
	if (!error) {
		response.number(static_cast<uint32_t>(entries.size()));
		for (const auto &entry : entries) {
			response.bytes(&entry.classGuid, sizeof(entry.classGuid));
			response.number(entry.deviceInstance);
		}
	}
	return response.write();
}

bool knownFolderPath(const WCHAR *identity, const WCHAR *flagsText, const WCHAR *user) {
	std::string bytes;
	if (!decodeHex(identity, bytes, true) || bytes.size() != sizeof(GUID))
		return false;
	WCHAR *end = nullptr;
	const auto flags = wcstoull(flagsText, &end, 10);
	if (!*flagsText || *end || flags > 0xffffffffULL)
		return false;
	HANDLE token = nullptr;
	if (wcscmp(user, L"default") == 0)
		token = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1));
	else if (wcscmp(user, L"current") != 0)
		return false;
	GUID id{};
	std::memcpy(&id, bytes.data(), sizeof(id));
	WCHAR *path = nullptr;
	const HRESULT status = SHGetKnownFolderPath(id, static_cast<DWORD>(flags), token, &path);
	Response response;
	response.header(status);
	if (SUCCEEDED(status))
		response.bytes(path, wcslen(path) * sizeof(WCHAR));
	CoTaskMemFree(path);
	return response.write();
}

bool bestRoute(WCHAR **parameters) {
	static_assert(sizeof(SOCKADDR_INET) == 28 && sizeof(MIB_IPFORWARD_ROW2) == 104);
	NET_LUID luid{};
	WCHAR *end = nullptr;
	const bool hasLuid = wcscmp(parameters[0], L"none") != 0;
	if (hasLuid) {
		luid.Value = wcstoull(parameters[0], &end, 10);
		if (!*parameters[0] || *end)
			return false;
	}
	ULONG numbers[2]{};
	for (unsigned i = 0; i < 2; ++i) {
		const auto value = wcstoull(parameters[i + 1], &end, 10);
		if (!*parameters[i + 1] || *end || value > 0xffffffffULL)
			return false;
		numbers[i] = static_cast<ULONG>(value);
	}
	std::string sourceBytes, destinationBytes;
	if (!decodeHex(parameters[3], sourceBytes, true) || !decodeHex(parameters[4], destinationBytes, true) ||
		(!sourceBytes.empty() && sourceBytes.size() != sizeof(SOCKADDR_INET)) ||
		destinationBytes.size() != sizeof(SOCKADDR_INET))
		return false;
	SOCKADDR_INET source{}, destination{}, bestSource{};
	if (!sourceBytes.empty())
		std::memcpy(&source, sourceBytes.data(), sizeof(source));
	std::memcpy(&destination, destinationBytes.data(), sizeof(destination));
	MIB_IPFORWARD_ROW2 route{};
	const DWORD status = GetBestRoute2(hasLuid ? &luid : nullptr, numbers[0], sourceBytes.empty() ? nullptr : &source,
									   &destination, numbers[1], &route, &bestSource);
	Response response;
	response.header(status);
	if (status == NO_ERROR) {
		response.bytes(&route, sizeof(route));
		response.bytes(&bestSource, sizeof(bestSource));
	}
	return response.write();
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

bool memoryResourceState() {
	HANDLE notifications[2] = {CreateMemoryResourceNotification(LowMemoryResourceNotification), nullptr};
	DWORD error = notifications[0] ? ERROR_SUCCESS : GetLastError();
	if (!error) {
		notifications[1] = CreateMemoryResourceNotification(HighMemoryResourceNotification);
		if (!notifications[1])
			error = GetLastError();
	}
	BOOL states[2]{};
	for (unsigned i = 0; i < 2 && !error; ++i)
		if (!QueryMemoryResourceNotification(notifications[i], &states[i]))
			error = GetLastError();
	for (HANDLE notification : notifications)
		if (notification)
			CloseHandle(notification);
	Response response;
	response.header(error);
	if (!error) {
		response.number(states[0] ? 1 : 0);
		response.number(states[1] ? 1 : 0);
	}
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

bool statusError(const WCHAR *statusText) {
	WCHAR *end = nullptr;
	const auto status = wcstoull(statusText, &end, 10);
	if (!*statusText || *end || status > UINT32_MAX)
		return false;
	using ConvertStatus = ULONG(WINAPI *)(LONG);
	const auto module = GetModuleHandleW(L"ntdll.dll");
	const auto convert =
		reinterpret_cast<ConvertStatus>(reinterpret_cast<void *>(GetProcAddress(module, "RtlNtStatusToDosError")));
	Response response;
	response.header(convert ? ERROR_SUCCESS : GetLastError());
	if (convert)
		response.number(convert(static_cast<LONG>(status)));
	return response.write();
}

bool systemQuery(const WCHAR *classText, const WCHAR *lengthText) {
	WCHAR *end = nullptr;
	const auto informationClass = wcstoull(classText, &end, 10);
	if (!*classText || *end || (informationClass != 3 && informationClass != 8))
		return false;
	const auto length = wcstoull(lengthText, &end, 10);
	if (!*lengthText || *end || length > 1024 * 1024)
		return false;
	using QuerySystem = LONG(WINAPI *)(ULONG, PVOID, ULONG, PULONG);
	const auto query = reinterpret_cast<QuerySystem>(
		reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
	Response response;
	if (!query) {
		response.header(GetLastError());
		return response.write();
	}
	std::vector<BYTE> data(static_cast<size_t>(length ? length : 1));
	ULONG returned = 0;
	const LONG status = query(static_cast<ULONG>(informationClass), data.data(), static_cast<ULONG>(length), &returned);
	if (status >= 0 && returned > length) {
		response.header(ERROR_INVALID_DATA);
		return response.write();
	}
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(status));
	response.number(returned);
	response.bytes(data.data(), status >= 0 ? returned : 0);
	return response.write();
}

bool volumeQuery(const WCHAR *path, const WCHAR *classText, const WCHAR *lengthText) {
	WCHAR *end = nullptr;
	const auto informationClass = wcstoull(classText, &end, 10);
	if (!*classText || *end || informationClass > UINT32_MAX)
		return false;
	const auto length = wcstoull(lengthText, &end, 10);
	if (!*lengthText || *end || length > 1024 * 1024)
		return false;
	using QueryVolume = LONG(WINAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
	const auto query = reinterpret_cast<QueryVolume>(
		reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVolumeInformationFile")));
	Response response;
	if (!query) {
		response.header(GetLastError());
		return response.write();
	}
	HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		response.header(GetLastError());
		return response.write();
	}
	std::vector<BYTE> data(static_cast<size_t>(length));
	IO_STATUS_BLOCK block{};
	const LONG status = query(file, &block, data.empty() ? nullptr : data.data(), static_cast<ULONG>(length),
							  static_cast<ULONG>(informationClass));
	CloseHandle(file);
	if (block.Information > length) {
		response.header(ERROR_INVALID_DATA);
		return response.write();
	}
	response.header(ERROR_SUCCESS);
	response.number(static_cast<uint32_t>(status));
	response.number(static_cast<uint32_t>(block.Information));
	response.number(0);
	response.bytes(data.data(), static_cast<size_t>(block.Information));
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

bool dispatch(int argc, WCHAR **argv) {
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
	else if (argc == 5 && wcscmp(argv[1], L"known-folder-path") == 0)
		written = knownFolderPath(argv[2], argv[3], argv[4]);
	else if (argc == 8 && wcscmp(argv[1], L"lc-map-string-ex") == 0)
		written = lcMapStringEx(argv + 2);
	else if (argc == 2 && wcscmp(argv[1], L"is-char-alpha-w-table") == 0)
		written = alphabeticCharacterTable();
	else if (argc == 5 && wcscmp(argv[1], L"char-upper-buff-w") == 0)
		written = upperCharacterBuffer(argv + 2);
	else if (argc == 8 && wcscmp(argv[1], L"compare-string-ex") == 0)
		written = compareStringEx(argv + 2);
	else if (argc == 10 && wcscmp(argv[1], L"find-nls-string-ex") == 0)
		written = findNlsStringEx(argv + 2);
	else if (argc == 4 && wcscmp(argv[1], L"cp-info-ex-w") == 0)
		written = cpInfoExW(argv[2], argv[3]);
	else if (argc == 4 && wcscmp(argv[1], L"is-valid-locale-name") == 0)
		written = isValidLocaleName(argv[2], argv[3]);
	else if (argc == 8 && wcscmp(argv[1], L"string-type-ex-a") == 0)
		written = stringTypeExA(argv + 2);
	else if (argc == 7 && wcscmp(argv[1], L"locale-info-ex") == 0)
		written = localeInfoEx(argv + 2);
	else if (argc == 6 && wcscmp(argv[1], L"resolve-locale-name") == 0)
		written = localeBufferSnapshot(argv + 2, LocaleSnapshotOperation::ResolveName);
	else if (argc == 10 && wcscmp(argv[1], L"file-mui-path") == 0)
		written = fileMuiPath(argv + 2);
	else if (argc == 5 && wcscmp(argv[1], L"user-preferred-ui-languages") == 0)
		written = userPreferredUiLanguages(argv[2], argv[3], argv[4]);
	else if (argc == 6 && wcscmp(argv[1], L"file-version-info-size-ex-w") == 0)
		written = fileVersionInfoSizeExW(argv[2], argv[3], argv[4], argv[5]);
	else if (argc == 8 && wcscmp(argv[1], L"file-version-info-ex-w") == 0)
		written = fileVersionInfoExW(argv + 2);
	else if (argc == 3 && wcscmp(argv[1], L"api-set-host") == 0)
		written = apiSetHost(argv[2]);
	else if (argc == 2 && wcscmp(argv[1], L"numa-highest-node-number") == 0)
		written = numaHighestNodeNumber();
	else if (argc == 2 && wcscmp(argv[1], L"time-zone-information") == 0)
		written = timeZoneInformation();
	else if (argc == 2 && wcscmp(argv[1], L"dynamic-time-zone-information") == 0)
		written = dynamicTimeZoneInformation();
	else if (argc == 2 && wcscmp(argv[1], L"environment-defaults") == 0)
		written = environmentDefaults();
	else if (argc == 3 && wcscmp(argv[1], L"network-connectivity") == 0)
		written = networkConnectivity(argv[2]);
	else if (argc == 3 && wcscmp(argv[1], L"ip-address-table") == 0 &&
			 (wcscmp(argv[2], L"0") == 0 || wcscmp(argv[2], L"1") == 0))
		written = ipAddressTable(wcscmp(argv[2], L"1") == 0);
	else if (argc == 7 && wcscmp(argv[1], L"best-route") == 0)
		written = bestRoute(argv + 2);
	else if (argc == 5 && wcscmp(argv[1], L"ip-adapter-addresses") == 0) {
		ULONG values[3]{};
		for (unsigned i = 0; i < 3; ++i) {
			WCHAR *end = nullptr;
			const auto value = wcstoull(argv[i + 2], &end, 10);
			if (!*argv[i + 2] || *end || value > 0xffffffffULL)
				return 1;
			values[i] = static_cast<ULONG>(value);
		}
		written = adapterAddresses<Response>(values[0], values[1], values[2]);
	} else if (argc == 2 && wcscmp(argv[1], L"memory-status") == 0)
		written = memoryStatus();
	else if (argc == 2 && wcscmp(argv[1], L"memory-resource-state") == 0)
		written = memoryResourceState();
	else if (argc == 4 && wcscmp(argv[1], L"system-metrics") == 0)
		written = systemMetrics(argv[2], argv[3]);
	else if (argc == 4 && wcscmp(argv[1], L"system-query") == 0)
		written = systemQuery(argv[2], argv[3]);
	else if (argc == 5 && wcscmp(argv[1], L"device-info-set-a") == 0)
		written = deviceInfoSetA(argv[2], argv[3], argv[4]);
	else if (argc == 5 && wcscmp(argv[1], L"volume-query") == 0)
		written = volumeQuery(argv[2], argv[3], argv[4]);
	else if (argc == 3 && wcscmp(argv[1], L"status-error") == 0)
		written = statusError(argv[2]);
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
	return written;
}

namespace {
constexpr size_t kMaxRequest = 64 * 1024;

bool readExact(void *buffer, size_t bytes) { return fread(buffer, 1, bytes, stdin) == bytes; }

uint32_t readNumber(const BYTE *bytes) {
	return bytes[0] | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

int serve() {
	streamResponse = true;
	_setmode(_fileno(stdin), _O_BINARY);
	for (;;) {
		BYTE length[4];
		const size_t count = fread(length, 1, sizeof(length), stdin);
		if (!count && feof(stdin))
			return 0;
		if (count != sizeof(length))
			return 1;
		const uint32_t size = readNumber(length);
		if (size < 4 || size > kMaxRequest)
			return 1;
		std::vector<BYTE> request(size);
		if (!readExact(request.data(), request.size()))
			return 1;
		const uint32_t argc = readNumber(request.data());
		if (!argc || argc > 32)
			return 1;
		size_t offset = 4;
		std::vector<std::wstring> arguments(argc + 1);
		for (unsigned index = 1; index <= argc; ++index) {
			if (size - offset < 4)
				return 1;
			const uint32_t bytes = readNumber(request.data() + offset);
			offset += 4;
			if (bytes > size - offset)
				return 1;
			if (bytes) {
				const char *text = reinterpret_cast<const char *>(request.data() + offset);
				if (memchr(text, 0, bytes))
					return 1;
				const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, bytes, nullptr, 0);
				if (characters <= 0)
					return 1;
				arguments[index].resize(characters);
				if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, bytes, arguments[index].data(),
										characters) != characters)
					return 1;
			}
			offset += bytes;
		}
		if (offset != size)
			return 1;
		std::vector<WCHAR *> argv;
		for (auto &argument : arguments)
			argv.push_back(argument.data());
		if (!dispatch(static_cast<int>(argv.size()), argv.data()))
			return 1;
	}
}
} // namespace

int wmain(int argc, WCHAR **argv) {
	_setmode(_fileno(stdout), _O_BINARY);
	if (argc == 2 && wcscmp(argv[1], L"--serve") == 0)
		return serve();
	return dispatch(argc, argv) ? 0 : 1;
}
