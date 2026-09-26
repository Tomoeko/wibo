#pragma once

#include "handles.h"
#include "minwinbase.h"

namespace kernel32 {
struct CompletionPacket {
	DWORD bytes;
	ULONG_PTR key;
	GUEST_PTR context;
	NTSTATUS status;
};

struct CompletionPortObject final : ObjectBase {
	static constexpr ObjectType kType = ObjectType::CompletionPort;
	struct Waiter {
		std::condition_variable cv;
		CompletionPacket packet{};
		bool ready = false;
	};
	std::mutex mutex;
	std::deque<CompletionPacket> packets;
	std::vector<Waiter *> waiters;
	const DWORD concurrency;
	size_t active = 0;
	bool closed = false;
	explicit CompletionPortObject(DWORD concurrency) : ObjectBase(kType), concurrency(concurrency) {}
	void dispatch(); // Caller holds mutex.
	bool post(CompletionPacket packet);
	void onLastHandleClosed() noexcept override;
};

struct CompletionBinding {
	Pin<CompletionPortObject> port;
	ULONG_PTR key;
	CompletionBinding(Pin<CompletionPortObject> port, ULONG_PTR key) : port(std::move(port)), key(key) {}
};

void detachCompletionThread();
class CompletionWait {
	Pin<CompletionPortObject> port;

  public:
	explicit CompletionWait(bool blocking = true);
	~CompletionWait();
	CompletionWait(const CompletionWait &) = delete;
	CompletionWait &operator=(const CompletionWait &) = delete;
};
} // namespace kernel32
