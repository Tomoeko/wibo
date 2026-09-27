#include "combase.h"

#include "common.h"
#include "context.h"
#include "modules.h"
#include "ole32.h"

namespace {
constexpr DWORD kApartmentThreaded = 2;
constexpr DWORD kDisableOle1Dde = 4;
constexpr DWORD kMultiThreaded = 0;
constexpr HRESULT kInvalidArgument = static_cast<HRESULT>(0x80070057);
} // namespace

namespace combase {

HRESULT WINAPI RoInitialize(RO_INIT_TYPE initType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RoInitialize(%d)\n", static_cast<int>(initType));
	switch (initType) {
	case RO_INIT_SINGLETHREADED:
		return ole32::CoInitializeEx(nullptr, kApartmentThreaded | kDisableOle1Dde);
	case RO_INIT_MULTITHREADED:
		return ole32::CoInitializeEx(nullptr, kMultiThreaded);
	default:
		return kInvalidArgument;
	}
}

void WINAPI RoUninitialize() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RoUninitialize()\n");
	ole32::CoUninitialize();
}

} // namespace combase

#include "combase_trampolines.h"

extern const wibo::ModuleStub lib_combase = {
	(const char *[]){"combase", nullptr},
	combaseThunkByName,
	nullptr,
};
