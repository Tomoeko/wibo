#include "wmi_proxy.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "oleaut32.h"
#include "strutil.h"
#include "system_provider.h"
#include "wmi_proxy_trampolines.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr HRESULT kInvalid = static_cast<HRESULT>(0x80041008);
constexpr HRESULT kUnsupported = static_cast<HRESULT>(0x8004100C);
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80041002);
constexpr HRESULT kNoInterface = static_cast<HRESULT>(0x80004002);
constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000E);
constexpr GUID kClassLocator{0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0, 0xAA, 0, 0x4B, 0x2E, 0x24}};
constexpr GUID kUnknown{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kLocator{0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0, 0xAA, 0, 0x4B, 0x2E, 0x24}};
constexpr GUID kServices{0x9556DC99, 0x828C, 0x11CF, {0xA3, 0x7E, 0, 0xAA, 0, 0x32, 0x40, 0xC7}};
constexpr GUID kEnumerator{0x027947E1, 0xD731, 0x11CE, {0xA3, 0x57, 0, 0, 0, 0, 0, 1}};
constexpr GUID kClassObject{0xDC12A681, 0x737F, 0x11CF, {0x88, 0x4D, 0, 0xAA, 0, 0x4B, 0x2E, 0x24}};

enum class Kind { Locator, Services, Enumerator, ClassObject };
struct Property {
	std::u16string name;
	uint32_t type = 0, flavor = 0, variantType = 0;
	HRESULT status = 0;
	std::vector<uint8_t> data;
};
using Record = std::vector<Property>;
using Records = std::vector<Record>;

struct State {
	Kind kind;
	std::mutex mutex{};
	ULONG references = 1; // Protected by the object registry mutex.
	std::string resource{}, query{};
	std::vector<std::string> security{};
	LONG flags = 0;
	bool loaded = false;
	HRESULT status = S_OK;
	std::shared_ptr<Records> records{};
	size_t cursor = 0, recordIndex = 0, propertyCursor = 0;
	bool enumerating = false;
	explicit State(Kind type) : kind(type) {}
};
std::mutex g_objectsMutex;
std::unordered_map<GUEST_PTR, std::shared_ptr<State>> g_objects;

bool sameGuid(const GUID *a, const GUID &b) { return a && std::memcmp(a, &b, sizeof(b)) == 0; }
const GUID &interfaceId(Kind kind) {
	switch (kind) {
	case Kind::Locator:
		return kLocator;
	case Kind::Services:
		return kServices;
	case Kind::Enumerator:
		return kEnumerator;
	case Kind::ClassObject:
		return kClassObject;
	}
	return kUnknown;
}

std::shared_ptr<State> find(GUEST_PTR self) {
	std::lock_guard lock(g_objectsMutex);
	auto it = g_objects.find(self);
	return it == g_objects.end() ? nullptr : it->second;
}

GUEST_PTR makeVtable(std::initializer_list<const char *> names) {
	auto *table = static_cast<GUEST_PTR *>(wibo::heap::guestMalloc(names.size() * sizeof(GUEST_PTR)));
	if (!table)
		return 0;
	size_t index = 0;
	for (const char *name : names)
		table[index++] = toGuestPtr(wmi_proxyThunkByName(name));
	return toGuestPtr(table);
}

GUEST_PTR vtable(Kind kind) {
	static const GUEST_PTR locator = makeVtable({"QueryInterface", "AddRef", "Release", "ConnectServer"});
	static const GUEST_PTR services = makeVtable(
		{"QueryInterface",	   "AddRef",	   "Release",	   "Unavailable5", "Unavailable2", "Unavailable3",
		 "Unavailable6",	   "Unavailable6", "Unavailable5", "Unavailable5", "Unavailable5", "Unavailable5",
		 "Unavailable5",	   "Unavailable5", "Unavailable5", "Unavailable5", "Unavailable5", "Unavailable5",
		 "CreateInstanceEnum", "Unavailable5", "ExecQuery",	   "Unavailable6", "Unavailable6", "Unavailable6",
		 "Unavailable8",	   "Unavailable8"});
	static const GUEST_PTR enumerator =
		makeVtable({"QueryInterface", "AddRef", "Release", "Reset", "Next", "Unavailable3", "Clone", "Skip"});
	static const GUEST_PTR object =
		makeVtable({"QueryInterface", "AddRef",		  "Release",	  "Unavailable2",	  "Get",
					"Unavailable5",	  "Unavailable2", "Unavailable5", "BeginEnumeration", "NextProperty",
					"EndEnumeration", "Unavailable3", "Clone",		  "Unavailable3",	  "Unavailable3",
					"Unavailable3",	  "Unavailable3", "Unavailable3", "Unavailable2",	  "Unavailable5",
					"Unavailable5",	  "Unavailable2", "Unavailable2", "Unavailable5",	  "Unavailable1",
					"Unavailable3",	  "Unavailable3"});
	switch (kind) {
	case Kind::Locator:
		return locator;
	case Kind::Services:
		return services;
	case Kind::Enumerator:
		return enumerator;
	case Kind::ClassObject:
		return object;
	}
	return 0;
}

HRESULT makeObject(std::shared_ptr<State> state, GUEST_PTR *result) {
	const GUEST_PTR table = vtable(state->kind);
	if (!table)
		return kOutOfMemory;
	auto *object = static_cast<GUEST_PTR *>(wibo::heap::guestMalloc(sizeof(GUEST_PTR)));
	if (!object)
		return kOutOfMemory;
	*object = table;
	*result = toGuestPtr(object);
	std::lock_guard lock(g_objectsMutex);
	g_objects.emplace(*result, std::move(state));
	return S_OK;
}

bool encode(LPCWSTR text, std::string &result) {
	return text && wibo::provider::encodeUtf8(
					   std::u16string_view(reinterpret_cast<const char16_t *>(text), wstrlen(text)), result);
}

size_t scalarSize(uint32_t type) {
	switch (type) {
	case 0:
	case 1:
		return 0;
	case 16:
	case 17:
		return 1;
	case 2:
	case 11:
	case 18:
		return 2;
	case 3:
	case 4:
	case 10:
	case 19:
	case 22:
	case 23:
		return 4;
	case 5:
	case 6:
	case 7:
	case 20:
	case 21:
		return 8;
	default:
		return SIZE_MAX;
	}
}

HRESULT load(State &state, LONG timeout) {
	if (state.loaded)
		return state.status;
	if (timeout == 0)
		return 0x40004; // WBEM_S_TIMEDOUT; no synchronous work for a polling call.
	std::vector<uint8_t> response;
	const int limit = timeout < 0 ? 10000 : std::min<LONG>(timeout, 10000);
	std::vector<std::string> arguments{"management-query", state.resource, state.query};
	arguments.insert(arguments.end(), state.security.begin(), state.security.end());
	bool timedOut = false;
	if (!wibo::provider::request(arguments, response, limit, &timedOut))
		return timedOut && timeout >= 0 ? 0x40004 : wibo::provider::kUnavailable;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t count = 0;
	if (!reader.header(status) || !reader.number(count) || count > 4096)
		return wibo::provider::kUnavailable;
	if (status < 0) {
		if (count || !reader.done())
			return wibo::provider::kUnavailable;
		state.loaded = true;
		return state.status = status;
	}
	if (status != S_OK)
		return wibo::provider::kUnavailable;
	auto records = std::make_shared<Records>();
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t properties = 0;
		if (!reader.number(properties) || properties > 4096)
			return wibo::provider::kUnavailable;
		Record record;
		for (uint32_t j = 0; j < properties; ++j) {
			Property property;
			uint32_t propertyStatus = 0;
			if (!reader.text(property.name) || property.name.find(u'\0') != std::u16string::npos ||
				!reader.number(property.type) || !reader.number(property.flavor) ||
				!reader.number(property.variantType) || !reader.number(propertyStatus) || !reader.bytes(property.data))
				return wibo::provider::kUnavailable;
			property.status = static_cast<HRESULT>(propertyStatus);
			if (property.status == S_OK &&
				(property.variantType == 8 ? property.data.size() % 2 != 0
										   : property.data.size() != scalarSize(property.variantType)))
				return wibo::provider::kUnavailable;
			if (property.status > 0 || property.variantType > 0xFFFF)
				return wibo::provider::kUnavailable;
			record.push_back(std::move(property));
		}
		records->push_back(std::move(record));
	}
	if (!reader.done())
		return wibo::provider::kUnavailable;
	state.records = std::move(records);
	state.loaded = true;
	return state.status = S_OK;
}

HRESULT copyValue(const Property &property, AutomationVariant *value, LONG *type, LONG *flavor) {
	if (property.status < 0)
		return property.status;
	if (value) {
		AutomationVariant converted{};
		converted.type = static_cast<WORD>(property.variantType);
		if (property.variantType == 8) {
			// The wire buffer is byte aligned. Copy into aligned UTF-16 storage first.
			std::vector<uint16_t> text(property.data.size() / 2);
			if (!text.empty())
				std::memcpy(text.data(), property.data.data(), property.data.size());
			LPWSTR allocated = oleaut32::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
			if (!allocated)
				return kOutOfMemory;
			converted.value.pointer = toGuestPtr(allocated);
		} else if (!property.data.empty()) {
			std::memcpy(&converted.value.scalar, property.data.data(), property.data.size());
		}
		*value = converted;
	}
	if (type)
		*type = static_cast<LONG>(property.type);
	if (flavor)
		*flavor = static_cast<LONG>(property.flavor);
	return S_OK;
}

bool equalName(std::u16string_view left, std::u16string_view right) {
	if (left.size() != right.size())
		return false;
	for (size_t i = 0; i < left.size(); ++i) {
		if (wcharToLower(left[i]) != wcharToLower(right[i]))
			return false;
	}
	return true;
}
} // namespace

namespace wibo::management {
HRESULT setProxyBlanket(GUEST_PTR proxy, DWORD authentication, DWORD authorization, LPCWSTR principal, DWORD level,
						DWORD impersonation, GUEST_PTR identity, DWORD capabilities) {
	auto state = find(proxy);
	if (!state || (state->kind != Kind::Services && state->kind != Kind::Enumerator))
		return kNoInterface;
	if (principal || identity)
		return kUnsupported;
	std::lock_guard lock(state->mutex);
	std::vector<std::string> settings{std::to_string(authentication), std::to_string(authorization),
									  std::to_string(level), std::to_string(impersonation),
									  std::to_string(capabilities)};
	std::vector<std::string> arguments{"management-connect", state->resource};
	arguments.insert(arguments.end(), settings.begin(), settings.end());
	std::vector<uint8_t> response;
	if (!provider::request(arguments, response))
		return provider::kUnavailable;
	provider::Reader reader(response);
	int32_t status = 0;
	uint32_t count = 0;
	if (!reader.header(status) || !reader.number(count) || count || !reader.done())
		return provider::kUnavailable;
	if (status == S_OK)
		state->security = std::move(settings);
	return status;
}

HRESULT createLocator(const GUID *classId, LPVOID outer, DWORD context, const GUID *iid, GUEST_PTR *result) {
	if (!result)
		return kPointer;
	*result = 0;
	if (!classId || !iid)
		return kPointer;
	if (!sameGuid(classId, kClassLocator) || !(context & 1) || !provider::configured())
		return static_cast<HRESULT>(0x80040154); // REGDB_E_CLASSNOTREG
	if (outer)
		return static_cast<HRESULT>(0x80040110); // CLASS_E_NOAGGREGATION
	if (!sameGuid(iid, kLocator) && !sameGuid(iid, kUnknown))
		return kNoInterface;
	return makeObject(std::make_shared<State>(Kind::Locator), result);
}
} // namespace wibo::management

namespace wmi_proxy {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	if (!result)
		return kPointer;
	*result = 0;
	std::lock_guard lock(g_objectsMutex);
	auto it = g_objects.find(self);
	if (it == g_objects.end() || !iid)
		return kPointer;
	if (!sameGuid(iid, kUnknown) && !sameGuid(iid, interfaceId(it->second->kind)))
		return kNoInterface;
	++it->second->references;
	*result = self;
	return S_OK;
}
ULONG WINAPI AddRef(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	std::lock_guard lock(g_objectsMutex);
	auto it = g_objects.find(self);
	return it == g_objects.end() ? 0 : ++it->second->references;
}
ULONG WINAPI Release(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	std::lock_guard lock(g_objectsMutex);
	auto it = g_objects.find(self);
	if (it == g_objects.end())
		return 0;
	const ULONG remaining = --it->second->references;
	if (!remaining) {
		g_objects.erase(it);
		wibo::heap::guestFree(fromGuestPtr(self));
	}
	return remaining;
}
HRESULT WINAPI ConnectServer(GUEST_PTR self, LPCWSTR resource, LPCWSTR user, LPCWSTR password, LPCWSTR locale,
							 LONG flags, LPCWSTR authority, GUEST_PTR context, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IWbemLocator::ConnectServer(%p, flags=0x%x)\n", fromGuestPtr(self), flags);
	if (!result)
		return kPointer;
	*result = 0;
	auto object = find(self);
	if (!object || object->kind != Kind::Locator || !resource || !*resource || (flags & ~0x80))
		return kInvalid;
	if (user || password || locale || authority || context)
		return kUnsupported;
	auto services = std::make_shared<State>(Kind::Services);
	if (!encode(resource, services->resource))
		return kInvalid;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"management-connect", services->resource}, response))
		return wibo::provider::kUnavailable;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t count = 0;
	if (!reader.header(status) || !reader.number(count) || count || !reader.done())
		return wibo::provider::kUnavailable;
	return status == S_OK ? makeObject(std::move(services), result) : status;
}
HRESULT WINAPI ExecQuery(GUEST_PTR self, LPCWSTR language, LPCWSTR query, LONG flags, GUEST_PTR context,
						 GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IWbemServices::ExecQuery(%p, flags=0x%x)\n", fromGuestPtr(self), flags);
	if (!result)
		return kPointer;
	*result = 0;
	auto services = find(self);
	if (!services || services->kind != Kind::Services || !query || !*query || !language)
		return kInvalid;
	if (context || (flags & ~0x30))
		return kUnsupported;
	if (!equalName(std::u16string_view(reinterpret_cast<const char16_t *>(language), wstrlen(language)), u"WQL"))
		return static_cast<HRESULT>(0x80041018);
	auto enumeration = std::make_shared<State>(Kind::Enumerator);
	{
		std::lock_guard lock(services->mutex);
		enumeration->resource = services->resource;
		enumeration->security = services->security;
	}
	enumeration->flags = flags;
	if (!encode(query, enumeration->query))
		return kInvalid;
	if (!(flags & 0x10)) {
		const HRESULT status = load(*enumeration, -1);
		if (status != S_OK)
			return status;
	}
	return makeObject(std::move(enumeration), result);
}
HRESULT WINAPI CreateInstanceEnum(GUEST_PTR self, LPCWSTR className, LONG flags, GUEST_PTR context, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IWbemServices::CreateInstanceEnum(%p, flags=0x%x)\n", fromGuestPtr(self), flags);
	if (!result)
		return kPointer;
	*result = 0;
	if (!className || !*className)
		return kInvalid;
	std::u16string query = u"SELECT * FROM ";
	for (const auto *cursor = className; *cursor; ++cursor) {
		if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') ||
			  (*cursor >= '0' && *cursor <= '9' && cursor != className) || *cursor == '_'))
			return kInvalid;
		query.push_back(static_cast<char16_t>(*cursor));
	}
	return ExecQuery(self, reinterpret_cast<LPCWSTR>(u"WQL"), reinterpret_cast<LPCWSTR>(query.c_str()), flags, context,
					 result);
}
HRESULT WINAPI Reset(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto state = find(self);
	if (!state || state->kind != Kind::Enumerator)
		return kInvalid;
	std::lock_guard lock(state->mutex);
	if (state->flags & 0x20)
		return static_cast<HRESULT>(0x80041024);
	state->cursor = 0;
	return S_OK;
}
HRESULT WINAPI Next(GUEST_PTR self, LONG timeout, ULONG count, GUEST_PTR *objects, ULONG *returned) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IEnumWbemClassObject::Next(%p, %d, %u)\n", fromGuestPtr(self), timeout, count);
	if (!returned)
		return kPointer;
	*returned = 0;
	auto state = find(self);
	if (!state || state->kind != Kind::Enumerator || !objects || !count || timeout < -1)
		return kInvalid;
	std::lock_guard lock(state->mutex);
	const HRESULT status = load(*state, timeout);
	if (status != S_OK)
		return status;
	while (*returned < count && state->cursor < state->records->size()) {
		auto object = std::make_shared<State>(Kind::ClassObject);
		object->records = state->records;
		object->recordIndex = state->cursor;
		const HRESULT created = makeObject(std::move(object), objects + *returned);
		if (created != S_OK)
			return created;
		++state->cursor;
		++*returned;
	}
	return *returned == count ? S_OK : 1;
}
HRESULT WINAPI Clone(GUEST_PTR self, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	if (!result)
		return kPointer;
	*result = 0;
	auto state = find(self);
	if (!state || (state->kind != Kind::Enumerator && state->kind != Kind::ClassObject))
		return kInvalid;
	std::lock_guard lock(state->mutex);
	if (state->kind == Kind::Enumerator && (state->flags & 0x20))
		return static_cast<HRESULT>(0x80041024);
	auto copy = std::make_shared<State>(state->kind);
	copy->resource = state->resource;
	copy->query = state->query;
	copy->security = state->security;
	copy->flags = state->flags;
	copy->loaded = state->loaded;
	copy->status = state->status;
	copy->records = state->records;
	copy->cursor = state->cursor;
	copy->recordIndex = state->recordIndex;
	return makeObject(std::move(copy), result);
}
HRESULT WINAPI Skip(GUEST_PTR self, LONG timeout, ULONG count) {
	HOST_CONTEXT_GUARD();
	auto state = find(self);
	if (!state || state->kind != Kind::Enumerator || timeout < -1)
		return kInvalid;
	std::lock_guard lock(state->mutex);
	const HRESULT status = load(*state, timeout);
	if (status != S_OK)
		return status;
	const size_t skipped = std::min<size_t>(count, state->records->size() - state->cursor);
	state->cursor += skipped;
	return skipped == count ? S_OK : 1;
}
HRESULT WINAPI Get(GUEST_PTR self, LPCWSTR name, LONG flags, AutomationVariant *value, LONG *type, LONG *flavor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IWbemClassObject::Get(%p, flags=0x%x)\n", fromGuestPtr(self), flags);
	auto state = find(self);
	if (!state || state->kind != Kind::ClassObject || !name || flags)
		return kInvalid;
	const std::u16string_view wanted(reinterpret_cast<const char16_t *>(name), wstrlen(name));
	for (const auto &property : (*state->records)[state->recordIndex]) {
		if (equalName(property.name, wanted))
			return copyValue(property, value, type, flavor);
	}
	return kNotFound;
}
HRESULT WINAPI BeginEnumeration(GUEST_PTR self, LONG flags) {
	HOST_CONTEXT_GUARD();
	auto state = find(self);
	if (!state || state->kind != Kind::ClassObject)
		return kInvalid;
	if (flags)
		return kUnsupported;
	std::lock_guard lock(state->mutex);
	state->propertyCursor = 0;
	state->enumerating = true;
	return S_OK;
}
HRESULT WINAPI NextProperty(GUEST_PTR self, LONG flags, GUEST_PTR *name, AutomationVariant *value, LONG *type,
							LONG *flavor) {
	HOST_CONTEXT_GUARD();
	if (name)
		*name = 0;
	auto state = find(self);
	if (!state || state->kind != Kind::ClassObject || flags)
		return kInvalid;
	std::lock_guard lock(state->mutex);
	if (!state->enumerating)
		return kInvalid;
	const Record &record = (*state->records)[state->recordIndex];
	if (state->propertyCursor == record.size())
		return 0x40005;
	const Property &property = record[state->propertyCursor];
	LPWSTR allocated = nullptr;
	if (name) {
		allocated = oleaut32::SysAllocStringLen(reinterpret_cast<LPCWSTR>(property.name.data()),
												static_cast<UINT>(property.name.size()));
		if (!allocated)
			return kOutOfMemory;
	}
	const HRESULT status = copyValue(property, value, type, flavor);
	if (status != S_OK) {
		oleaut32::SysFreeString(allocated);
		return status;
	}
	if (name)
		*name = toGuestPtr(allocated);
	++state->propertyCursor;
	return S_OK;
}
HRESULT WINAPI EndEnumeration(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto state = find(self);
	if (!state || state->kind != Kind::ClassObject)
		return kInvalid;
	std::lock_guard lock(state->mutex);
	state->enumerating = false;
	return S_OK;
}
HRESULT WINAPI Unavailable1(GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
HRESULT WINAPI Unavailable2(GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
HRESULT WINAPI Unavailable3(GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
HRESULT WINAPI Unavailable5(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
HRESULT WINAPI Unavailable6(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
HRESULT WINAPI Unavailable8(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnsupported;
}
} // namespace wmi_proxy
