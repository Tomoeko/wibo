#include "setupapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "system_provider.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace {
constexpr DWORD kAllClasses = 4;
constexpr DWORD kInvalidUserBuffer = 1784;
constexpr GUEST_PTR kMaxIdentifier = (std::numeric_limits<GUEST_PTR>::max() - 1) >> 2;

struct DeviceInfo {
	GUID classGuid{};
	DWORD instance = 0;
	GUEST_PTR identifier = 0;
};

std::mutex g_deviceSetsMutex;
std::unordered_map<GUEST_PTR, std::vector<DeviceInfo>> g_deviceSets;
GUEST_PTR g_nextIdentifier = 0;

DWORD deviceSnapshot(const GUID *classGuid, LPCSTR enumerator, DWORD flags, std::vector<DeviceInfo> &devices) {
	const std::string classArgument = classGuid ? wibo::provider::encodeBytes(std::string_view(
													  reinterpret_cast<const char *>(classGuid), sizeof(*classGuid)))
												: "-";
	const std::string enumeratorArgument = enumerator ? wibo::provider::encodeBytes(enumerator) : "-";
	std::vector<uint8_t> response;
	bool timedOut = false;
	if (!wibo::provider::request({"device-info-set-a", classArgument, enumeratorArgument, std::to_string(flags)},
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

GUEST_PTR nextIdentifier() { return (++g_nextIdentifier << 2) | 1; }
} // namespace

namespace setupapi {
static_assert(sizeof(SP_DEVINFO_DATA) == (sizeof(GUEST_PTR) == 8 ? 32 : 28));

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
	std::vector<DeviceInfo> devices;
	const DWORD error = deviceSnapshot(classGuid, enumerator, flags, devices);
	if (error) {
		kernel32::setLastError(error);
		return INVALID_HANDLE_VALUE;
	}
	std::lock_guard lock(g_deviceSetsMutex);
	if (devices.size() + 1 > kMaxIdentifier - g_nextIdentifier) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	const GUEST_PTR set = nextIdentifier();
	for (auto &device : devices)
		device.identifier = nextIdentifier();
	g_deviceSets.emplace(set, std::move(devices));
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
	if (memberIndex >= it->second.size()) {
		kernel32::setLastError(ERROR_NO_MORE_ITEMS);
		return FALSE;
	}
	const auto &device = it->second[memberIndex];
	*deviceInfoData = {sizeof(*deviceInfoData), device.classGuid, device.instance, device.identifier};
	return TRUE;
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
