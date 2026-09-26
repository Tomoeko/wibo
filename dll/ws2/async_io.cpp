#include "ws2/async_io.h"

#include "errors.h"
#include "kernel32/overlapped_util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {
void complete(ws2::detail::SocketIoRequest &request) {
	request.release();
	{
		std::lock_guard lock(request.socket->ioMutex);
		kernel32::detail::signalOverlappedCompletion(request.binding, request.overlapped, request.status,
													 request.bytes);
	}
	request.socket->overlappedCv.notify_all();
}

class SocketIoWorker;
std::atomic<SocketIoWorker *> g_worker = nullptr;
class SocketIoWorker {
	std::mutex mutex;
	std::vector<std::unique_ptr<ws2::detail::SocketIoRequest>> requests;
	std::atomic<bool> stopping = false;
	int wakePipe[2] = {-1, -1};
	std::thread thread;

	void run() {
		while (true) {
			std::vector<pollfd> descriptors;
			std::vector<ws2::detail::SocketIoRequest *> snapshot;
			bool immediate = stopping.load();
			{
				std::lock_guard lock(mutex);
				descriptors.push_back({wakePipe[0], POLLIN, 0});
				std::unordered_map<const ws2::detail::Socket *, unsigned> ordered;
				for (const auto &request : requests) {
					immediate |= request->isCancelled();
					snapshot.push_back(request.get());
					const auto group = static_cast<unsigned>(request->order);
					auto &groups = ordered[request->socket.get()];
					const bool blocked = (groups & group) != 0;
					groups |= group;
					descriptors.push_back({blocked ? -1 : request->descriptor(), request->events(), 0});
				}
			}
			int ready;
			do {
				ready = ::poll(descriptors.data(), descriptors.size(), immediate ? 0 : -1);
			} while (ready < 0 && errno == EINTR);
			if (descriptors[0].revents) {
				std::array<char, 128> bytes{};
				while (::read(wakePipe[0], bytes.data(), bytes.size()) > 0) {
				}
			}
			std::vector<std::unique_ptr<ws2::detail::SocketIoRequest>> completed;
			{
				std::lock_guard lock(mutex);
				for (size_t i = 0; i < snapshot.size(); ++i) {
					auto *request = snapshot[i];
					request->cancelled |= stopping.load();
					if (descriptors[i + 1].revents || request->isCancelled()) {
						if (request->isCancelled() || request->process()) {
							if (request->isCancelled())
								request->status = STATUS_CANCELLED;
							auto found = std::find_if(requests.begin(), requests.end(),
													  [=](const auto &entry) { return entry.get() == request; });
							completed.push_back(std::move(*found));
							requests.erase(found);
						}
					}
				}
			}
			for (const auto &request : completed)
				complete(*request);
			if (stopping) {
				std::lock_guard lock(mutex);
				if (requests.empty())
					break;
			}
		}
	}

  public:
	SocketIoWorker() {
		if (::pipe(wakePipe) < 0)
			return;
		for (int descriptor : wakePipe) {
			if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
				::close(wakePipe[0]);
				::close(wakePipe[1]);
				wakePipe[0] = wakePipe[1] = -1;
				return;
			}
		}
		thread = std::thread(&SocketIoWorker::run, this);
	}
	~SocketIoWorker() {
		g_worker = nullptr;
		stopping = true;
		wake();
		if (thread.joinable())
			thread.join();
		for (int descriptor : wakePipe)
			if (descriptor >= 0)
				::close(descriptor);
	}
	bool enqueue(std::unique_ptr<ws2::detail::SocketIoRequest> request) {
		std::lock_guard lock(mutex);
		if (!thread.joinable() || stopping || requests.size() >= 4096)
			return false;
		__atomic_store_n(&request->overlapped->InternalHigh, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&request->overlapped->Internal, static_cast<ULONG_PTR>(STATUS_PENDING), __ATOMIC_RELEASE);
		kernel32::detail::resetOverlappedEvent(request->overlapped);
		requests.push_back(std::move(request));
		wake();
		return true;
	}
	void wake() const {
		const char byte = 1;
		if (wakePipe[1] >= 0)
			while (::write(wakePipe[1], &byte, 1) < 0 && errno == EINTR) {
			}
	}
	void cancelThread(pthread_t owner) {
		std::lock_guard lock(mutex);
		for (const auto &request : requests)
			if (pthread_equal(owner, request->owner))
				request->cancelled = true;
		wake();
	}
};
SocketIoWorker &worker() {
	static SocketIoWorker instance;
	g_worker = &instance;
	return instance;
}
} // namespace

namespace ws2::detail {
bool queueSocketIo(std::unique_ptr<SocketIoRequest> request) { return worker().enqueue(std::move(request)); }
void wakeSocketIo() {
	if (auto *worker = g_worker.load())
		worker->wake();
}
void cancelSocketIoForThread(pthread_t owner) {
	if (auto *worker = g_worker.load())
		worker->cancelThread(owner);
}
} // namespace ws2::detail
