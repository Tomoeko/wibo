#pragma once

#include "errors.h"
#include "kernel32/completion_port.h"
#include "kernel32/minwinbase.h"
#include "ws2/internal.h"

namespace ws2::detail {
enum class SocketIoOrder : unsigned { Unordered = 0, Receive = 1, Send = 2 };
struct SocketIoRequest {
	std::shared_ptr<Socket> socket;
	std::shared_ptr<const kernel32::CompletionBinding> binding;
	OVERLAPPED *overlapped = nullptr;
	pthread_t owner = pthread_self();
	bool cancelled = false;
	NTSTATUS status = STATUS_SUCCESS;
	DWORD bytes = 0;
	SocketIoOrder order = SocketIoOrder::Unordered;
	virtual ~SocketIoRequest() = default;
	[[nodiscard]] virtual int descriptor() const { return socket->descriptor; }
	[[nodiscard]] virtual short events() const = 0;
	[[nodiscard]] virtual bool isCancelled() const { return cancelled || socket->closed; }
	virtual bool process() = 0;
	virtual void release() {}
};
bool queueSocketIo(std::unique_ptr<SocketIoRequest> request);
bool cancelSocketIo(const std::shared_ptr<Socket> &socket, const OVERLAPPED *overlapped);
} // namespace ws2::detail
