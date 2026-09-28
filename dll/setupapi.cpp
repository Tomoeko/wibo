#include "setupapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "system_provider.h"

#include <cstring>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
constexpr DWORD kAllClasses = 4;
constexpr DWORD kDeviceInterface = 0x10;
constexpr DWORD kInvalidUserBuffer = 1784;
constexpr GUEST_PTR kMaxIdentifier = (std::numeric_limits<GUEST_PTR>::max() - 1) >> 2;

struct DeviceInfo {
	GUID classGuid{};
	DWORD instance = 0;
	GUEST_PTR identifier = 0;
};

struct InterfaceInfo {
	GUID classGuid{};
	DWORD flags = 0;
	DWORD deviceInstance = 0;
	GUID deviceClassGuid{};
	std::u16string path;
	GUEST_PTR identifier = 0;
};

struct DeviceSet {
	std::vector<DeviceInfo> devices;
	std::vector<InterfaceInfo> interfaces;
	bool interfacesLoaded = false;
};

std::mutex g_deviceSetsMutex;
std::unordered_map<GUEST_PTR, DeviceSet> g_deviceSets;
GUEST_PTR g_nextIdentifier = 0;

std::vector<std::string> snapshotArguments(const GUID *classGuid, LPCSTR enumerator, DWORD flags,
													 std::string_view operation) {
	const std::string classArgument = classGuid ? wibo::provider::encodeBytes(std::string_view(
													  reinterpret_cast<const char *>(classGuid), sizeof(*classGuid)))
												: "-";
	const std::string enumeratorArgument = enumerator ? wibo::provider::encodeBytes(enumerator) : "-";
	return {std::string(operation), classArgument, enumeratorArgument, std::to_string(flags)};
}

DWORD deviceSnapshot(const GUID *classGuid, LPCSTR enumerator, DWORD flags, std::vector<DeviceInfo> &devices) {
	std::vector<uint8_t> response;
	bool timedOut = false;
	if (!wibo::provider::request(snapshotArguments(classGuid, enumerator, flags, "device-info-set-a"),
								 response, 10000, &timedOut))
		return timedOut ? ERROR_TIMEOUT : ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	if (status) {
		if (!reader.done())
			return ERROR_INVALID_DATA;
		return status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status);
	}
	uint32_t count = 0;
	// Each record carries a length-prefixed GUID and a device instance value.
	if (!reader.number(count) || count > wibo::provider::kMaxResponse / 24)
		return ERROR_INVALID_DATA;
	devices.reserve(count);
	std::vector<uint8_t> bytes;
	for (uint32_t index = 0; index < count; ++index) {
		DeviceInfo device;
		if (!reader.bytes(bytes) || bytes.size() != sizeof(device.classGuid) || !reader.number(device.instance))
			return ERROR_INVALID_DATA;
		std::memcpy(&device.classGuid, bytes.data(), sizeof(device.classGuid));
		devices.push_back(device);
	}
	return reader.done() ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

DWORD interfaceSnapshot(const GUID *classGuid, LPCSTR enumerator, DWORD flags, DeviceSet &deviceSet) {
	std::vector<uint8_t> response;
	bool timedOut = false;
	if (!wibo::provider::request(snapshotArguments(classGuid, enumerator, flags, "device-interface-set-a"),
								 response, 10000, &timedOut))
		return timedOut ? ERROR_TIMEOUT : ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	if (status) {
		if (!reader.done())
			return ERROR_INVALID_DATA;
		return status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status);
	}
	uint32_t count = 0;
	if (!reader.number(count) || count > wibo::provider::kMaxResponse / 24)
		return ERROR_INVALID_DATA;
	deviceSet.devices.reserve(count);
	std::vector<uint8_t> bytes;
	for (uint32_t index = 0; index < count; ++index) {
		DeviceInfo device;
		if (!reader.bytes(bytes) || bytes.size() != sizeof(GUID) || !reader.number(device.instance))
			return ERROR_INVALID_DATA;
		std::memcpy(&device.classGuid, bytes.data(), sizeof(GUID));
		deviceSet.devices.push_back(device);
	}
	if (!reader.number(count) || count > wibo::provider::kMaxResponse / 52)
		return ERROR_INVALID_DATA;
	deviceSet.interfaces.reserve(count);
	for (uint32_t index = 0; index < count; ++index) {
		InterfaceInfo info;
		if (!reader.bytes(bytes) || bytes.size() != sizeof(GUID))
			return ERROR_INVALID_DATA;
		std::memcpy(&info.classGuid, bytes.data(), sizeof(GUID));
		if (std::memcmp(&info.classGuid, classGuid, sizeof(GUID)) || !reader.number(info.flags) ||
			!reader.number(info.deviceInstance) || !reader.bytes(bytes) || bytes.size() != sizeof(GUID))
			return ERROR_INVALID_DATA;
		std::memcpy(&info.deviceClassGuid, bytes.data(), sizeof(GUID));
		if (!reader.bytes(bytes) || bytes.empty() || bytes.size() % sizeof(WCHAR) ||
			bytes.size() / sizeof(WCHAR) > 32767)
			return ERROR_INVALID_DATA;
		info.path.reserve(bytes.size() / sizeof(WCHAR));
		for (size_t offset = 0; offset < bytes.size(); offset += sizeof(WCHAR)) {
			const char16_t character = static_cast<char16_t>(bytes[offset] | (bytes[offset + 1] << 8));
			if (!character)
				return ERROR_INVALID_DATA;
			info.path.push_back(character);
		}
		deviceSet.interfaces.push_back(std::move(info));
	}
	return reader.done() ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

bool equalGuid(const GUID &left, const GUID &right) { return !std::memcmp(&left, &right, sizeof(GUID)); }

const DeviceInfo *findDevice(const DeviceSet &set, DWORD instance, const GUID &classGuid) {
	for (const auto &device : set.devices) {
		if (device.instance == instance && equalGuid(device.classGuid, classGuid))
			return &device;
	}
	return nullptr;
}

const InterfaceInfo *findInterface(const DeviceSet &set, GUEST_PTR identifier) {
	for (const auto &info : set.interfaces) {
		if (info.identifier == identifier)
			return &info;
	}
	return nullptr;
}

GUEST_PTR nextIdentifier() { return (++g_nextIdentifier << 2) | 1; }
} // namespace

namespace setupapi {
static_assert(sizeof(SP_DEVINFO_DATA) == (sizeof(GUEST_PTR) == 8 ? 32 : 28));
static_assert(sizeof(SP_DEVICE_INTERFACE_DATA) == (sizeof(GUEST_PTR) == 8 ? 32 : 28));
static_assert(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) == 4);
static_assert(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) == 4);

HANDLE WINAPI SetupDiGetClassDevsA(const GUID *classGuid, LPCSTR enumerator, HWND parent, DWORD flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiGetClassDevsA(%p, %p, %p, 0x%x)\n", classGuid, enumerator, parent, flags);
	if (!classGuid && !(flags & kAllClasses)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (parent) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return INVALID_HANDLE_VALUE;
	}
	DeviceSet deviceSet;
	const bool interfaceSet = classGuid && (flags & kDeviceInterface);
	DWORD error = interfaceSet ? interfaceSnapshot(classGuid, enumerator, flags, deviceSet)
						   : deviceSnapshot(classGuid, enumerator, flags, deviceSet.devices);
	if (!error && interfaceSet) {
		if (!error) {
			deviceSet.interfacesLoaded = true;
			for (const auto &info : deviceSet.interfaces) {
				if (!findDevice(deviceSet, info.deviceInstance, info.deviceClassGuid)) {
					error = ERROR_INVALID_DATA;
					break;
				}
			}
		}
	}
	if (error) {
		kernel32::setLastError(error);
		return INVALID_HANDLE_VALUE;
	}
	std::lock_guard lock(g_deviceSetsMutex);
	if (deviceSet.devices.size() + deviceSet.interfaces.size() + 1 > kMaxIdentifier - g_nextIdentifier) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	const GUEST_PTR set = nextIdentifier();
	for (auto &device : deviceSet.devices)
		device.identifier = nextIdentifier();
	for (auto &info : deviceSet.interfaces)
		info.identifier = nextIdentifier();
	g_deviceSets.emplace(set, std::move(deviceSet));
	return static_cast<HANDLE>(set);
}

BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE deviceInfoSet, DWORD memberIndex, SP_DEVINFO_DATA *deviceInfoData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiEnumDeviceInfo(%p, %u, %p)\n", deviceInfoSet, memberIndex, deviceInfoData);
	std::lock_guard lock(g_deviceSetsMutex);
	const auto it = g_deviceSets.find(static_cast<GUEST_PTR>(deviceInfoSet));
	if (it == g_deviceSets.end()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!deviceInfoData) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (deviceInfoData->cbSize != sizeof(*deviceInfoData)) {
		kernel32::setLastError(kInvalidUserBuffer);
		return FALSE;
	}
	if (memberIndex >= it->second.devices.size()) {
		kernel32::setLastError(ERROR_NO_MORE_ITEMS);
		return FALSE;
	}
	const auto &device = it->second.devices[memberIndex];
	*deviceInfoData = {sizeof(*deviceInfoData), device.classGuid, device.instance, device.identifier};
	return TRUE;
}

BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE deviceInfoSet, SP_DEVINFO_DATA *deviceInfoData,
		const GUID *interfaceClassGuid, DWORD memberIndex, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiEnumDeviceInterfaces(%p, %p, %p, %u, %p)\n", deviceInfoSet, deviceInfoData,
			  interfaceClassGuid, memberIndex, deviceInterfaceData);
	const bool validOutput = deviceInterfaceData && deviceInterfaceData->cbSize == sizeof(*deviceInterfaceData);
	if (validOutput) {
		std::memset(deviceInterfaceData, 0, sizeof(*deviceInterfaceData));
		deviceInterfaceData->cbSize = sizeof(*deviceInterfaceData);
	}
	std::lock_guard lock(g_deviceSetsMutex);
	const auto it = g_deviceSets.find(static_cast<GUEST_PTR>(deviceInfoSet));
	if (it == g_deviceSets.end()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!validOutput) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	GUEST_PTR deviceIdentifier = 0;
	if (deviceInfoData) {
		if (deviceInfoData->cbSize != sizeof(*deviceInfoData)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		deviceIdentifier = deviceInfoData->Reserved;
		bool found = false;
		for (const auto &device : it->second.devices) {
			if (device.identifier == deviceIdentifier) {
				found = true;
				break;
			}
		}
		if (!found) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	if (!it->second.interfacesLoaded) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!interfaceClassGuid) {
		kernel32::setLastError(it->second.interfaces.empty() ? ERROR_NO_MORE_ITEMS : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	DWORD currentIndex = 0;
	for (const auto &info : it->second.interfaces) {
		if (!equalGuid(info.classGuid, *interfaceClassGuid))
			continue;
		if (deviceInfoData) {
			const DeviceInfo *owner = findDevice(it->second, info.deviceInstance, info.deviceClassGuid);
			if (!owner || owner->identifier != deviceIdentifier)
				continue;
		}
		if (currentIndex++ != memberIndex)
			continue;
		*deviceInterfaceData = {sizeof(*deviceInterfaceData), info.classGuid, info.flags, info.identifier};
		return TRUE;
	}
	kernel32::setLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

template <typename DetailData>
BOOL interfaceDetail(HANDLE deviceInfoSet, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData,
		DetailData *detailData, DWORD detailDataSize, DWORD *requiredSize, SP_DEVINFO_DATA *deviceInfoData,
		bool wide) {
	std::lock_guard lock(g_deviceSetsMutex);
	const auto it = g_deviceSets.find(static_cast<GUEST_PTR>(deviceInfoSet));
	if (it == g_deviceSets.end()) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!deviceInterfaceData || deviceInterfaceData->cbSize != sizeof(*deviceInterfaceData)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const InterfaceInfo *info = findInterface(it->second, deviceInterfaceData->Reserved);
	if (!info) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DeviceInfo *owner = findDevice(it->second, info->deviceInstance, info->deviceClassGuid);
	if (!owner) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (deviceInfoData && deviceInfoData->cbSize != sizeof(*deviceInfoData)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD expectedDetailSize = sizeof(GUEST_PTR) == 8 ? 8 : (wide ? 6 : 5);
	if ((detailData && detailData->cbSize != expectedDetailSize) || (!detailData && detailDataSize)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!wide) {
		for (const char16_t character : info->path) {
			if (character > 0x7f) {
				kernel32::setLastError(ERROR_NOT_SUPPORTED);
				return FALSE;
			}
		}
	}
	const DWORD required = 4 + static_cast<DWORD>(info->path.size() + 1) * (wide ? sizeof(WCHAR) : sizeof(CHAR));
	if (requiredSize)
		*requiredSize = required;
	if (deviceInfoData)
		*deviceInfoData = {sizeof(*deviceInfoData), owner->classGuid, owner->instance, owner->identifier};
	if (!detailData || detailDataSize < required) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	auto *path = reinterpret_cast<uint8_t *>(detailData) + 4;
	if (wide) {
		std::memcpy(path, info->path.data(), info->path.size() * sizeof(WCHAR));
		std::memset(path + info->path.size() * sizeof(WCHAR), 0, sizeof(WCHAR));
	} else {
		for (size_t index = 0; index < info->path.size(); ++index)
			path[index] = static_cast<uint8_t>(info->path[index]);
		path[info->path.size()] = 0;
	}
	return TRUE;
}

BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HANDLE deviceInfoSet, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData,
		SP_DEVICE_INTERFACE_DETAIL_DATA_A *deviceInterfaceDetailData, DWORD deviceInterfaceDetailDataSize,
		DWORD *requiredSize, SP_DEVINFO_DATA *deviceInfoData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiGetDeviceInterfaceDetailA(%p, %p, %p, %u, %p, %p)\n", deviceInfoSet,
			  deviceInterfaceData, deviceInterfaceDetailData, deviceInterfaceDetailDataSize, requiredSize, deviceInfoData);
	return interfaceDetail(deviceInfoSet, deviceInterfaceData, deviceInterfaceDetailData,
						   deviceInterfaceDetailDataSize, requiredSize, deviceInfoData, false);
}

BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE deviceInfoSet, SP_DEVICE_INTERFACE_DATA *deviceInterfaceData,
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *deviceInterfaceDetailData, DWORD deviceInterfaceDetailDataSize,
		DWORD *requiredSize, SP_DEVINFO_DATA *deviceInfoData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiGetDeviceInterfaceDetailW(%p, %p, %p, %u, %p, %p)\n", deviceInfoSet,
			  deviceInterfaceData, deviceInterfaceDetailData, deviceInterfaceDetailDataSize, requiredSize, deviceInfoData);
	return interfaceDetail(deviceInfoSet, deviceInterfaceData, deviceInterfaceDetailData,
						   deviceInterfaceDetailDataSize, requiredSize, deviceInfoData, true);
}

BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE deviceInfoSet) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetupDiDestroyDeviceInfoList(%p)\n", deviceInfoSet);
	std::lock_guard lock(g_deviceSetsMutex);
	if (!g_deviceSets.erase(static_cast<GUEST_PTR>(deviceInfoSet))) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	kernel32::setLastError(ERROR_SUCCESS);
	return TRUE;
}

} // namespace setupapi

#include "setupapi_trampolines.h"

extern const wibo::ModuleStub lib_setupapi = {
	(const char *[]){
		"setupapi",
		nullptr,
	},
	setupapiThunkByName,
	nullptr,
};
