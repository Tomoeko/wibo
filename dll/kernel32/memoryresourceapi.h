#pragma once

#include "handles.h"

namespace kernel32 {

struct MemoryResourceMonitorSession;

struct MemoryResourceObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::MemoryResource;
	const DWORD notificationType;
	DWORD backendError = 0;		   // protected by m
	uint64_t sampleGeneration = 0; // protected by m
	std::shared_ptr<MemoryResourceMonitorSession> session;

	explicit MemoryResourceObject(DWORD type) : WaitableObject(kType), notificationType(type) {}
	void publish(bool state, DWORD error, uint64_t generation);
	void onLastHandleClosed() noexcept override;
};

HANDLE WINAPI CreateMemoryResourceNotification(DWORD notificationType);
BOOL WINAPI QueryMemoryResourceNotification(HANDLE notification, BOOL *state);

} // namespace kernel32
