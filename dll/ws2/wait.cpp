#include "ws2/internal.h"

#include "common.h"
#include "context.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <unordered_map>
#include <vector>
#ifdef __APPLE__
#include <sys/event.h>
#include <unistd.h>
#endif

namespace {
struct GuestTimeout {
	LONG seconds, microseconds;
};
static_assert(sizeof(GuestTimeout) == 8);
struct Target {
	SOCKET handle;
	std::shared_ptr<ws2::detail::Socket> socket;
	bool read = false, write = false, exception = false;
};
struct Set {
	void *output;
	std::vector<size_t> targets;
};

int waitForSockets(std::vector<pollfd> &descriptors, const std::vector<Target> &targets, int milliseconds) {
#ifdef __APPLE__
	// The host poll implementation does not reliably wake for urgent data.
	if (std::any_of(descriptors.begin(), descriptors.end(), [](const auto &fd) { return fd.events & POLLPRI; })) {
		struct Queue {
			int descriptor = ::kqueue();
			~Queue() {
				if (descriptor >= 0)
					::close(descriptor);
			}
		} queue;
		if (queue.descriptor < 0)
			return -1;
		std::vector<struct kevent> changes;
		for (size_t index = 0; index != descriptors.size(); ++index) {
			auto &fd = descriptors[index];
			fd.revents = 0;
			if (fd.fd < 0)
				continue;
			auto add = [&](int16_t filter, unsigned flags) {
				struct kevent event{};
				EV_SET(&event, fd.fd, filter, EV_ADD | EV_ONESHOT, flags, 0, reinterpret_cast<void *>(index));
				changes.push_back(event);
			};
			if (fd.events & POLLIN)
				add(EVFILT_READ, 0);
			if ((fd.events & POLLOUT) || ((fd.events & POLLPRI) && targets[index].socket->connecting.load()))
				add(EVFILT_WRITE, 0);
			if (fd.events & POLLPRI)
				add(EVFILT_EXCEPT, NOTE_OOB);
		}
		std::vector<struct kevent> events(std::max<size_t>(changes.size(), 1));
		timespec duration{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
		const int count = ::kevent(queue.descriptor, changes.data(), static_cast<int>(changes.size()), events.data(),
								   static_cast<int>(events.size()), milliseconds < 0 ? nullptr : &duration);
		if (count < 0)
			return -1;
		for (int index = 0; index != count; ++index) {
			const auto &event = events[index];
			auto &fd = descriptors[reinterpret_cast<uintptr_t>(event.udata)];
			if ((event.flags & EV_ERROR) && event.data) {
				errno = static_cast<int>(event.data);
				return -1;
			}
			if (event.filter == EVFILT_READ)
				fd.revents |= POLLIN;
			if (event.filter == EVFILT_WRITE)
				fd.revents |= POLLOUT;
			if (event.filter == EVFILT_EXCEPT && (event.fflags & NOTE_OOB))
				fd.revents |= POLLPRI;
			if (event.flags & EV_EOF) {
				fd.revents |= POLLHUP;
				if (event.fflags)
					fd.revents |= POLLERR;
			}
		}
		return count;
	}
#else
	(void)targets;
#endif
	return ::poll(descriptors.data(), descriptors.size(), milliseconds);
}
} // namespace

namespace ws2 {
int WINAPI __WSAFDIsSet(SOCKET handle, const WSA_FD_SET *set) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("__WSAFDIsSet(0x%llx, %p)\n", static_cast<unsigned long long>(handle), set);
	DWORD count = 0;
	if (!set) {
		detail::setLastError(10014);
		return 0;
	}
	std::memcpy(&count, set, sizeof(count));
	if (count > WSA_FD_SET::kMaxCount) {
		detail::setLastError(10022);
		return 0;
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(set) + offsetof(WSA_FD_SET, sockets);
	for (DWORD index = 0; index < count; ++index) {
		SOCKET value = 0;
		std::memcpy(&value, bytes + index * sizeof(value), sizeof(value));
		if (value == handle)
			return 1;
	}
	return 0;
}

int WINAPI select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("select(%d, %p, %p, %p, %p)\n", nfds, readfds, writefds, exceptfds, timeout);
	(void)nfds;
	if (!detail::requireStarted())
		return -1;
	GuestTimeout duration{};
	if (timeout) {
		std::memcpy(&duration, timeout, sizeof(duration));
		if (duration.seconds < 0 || duration.microseconds < 0 || duration.microseconds >= 1000000)
			return detail::failSocket(10022);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration.seconds) +
						  std::chrono::microseconds(duration.microseconds);
	std::array<Set, 3> sets{{{readfds, {}}, {writefds, {}}, {exceptfds, {}}}};
	std::vector<Target> targets;
	std::unordered_map<SOCKET, size_t> indices;
	for (auto &set : sets) {
		if (!set.output)
			continue;
		DWORD count = 0;
		std::memcpy(&count, set.output, sizeof(count));
		if (count > WSA_FD_SET::kMaxCount)
			return detail::failSocket(10022);
		const auto *bytes = static_cast<const uint8_t *>(set.output) + offsetof(WSA_FD_SET, sockets);
		for (DWORD index = 0; index != count; ++index) {
			SOCKET handle = 0;
			std::memcpy(&handle, bytes + index * sizeof(handle), sizeof(handle));
			auto [found, inserted] = indices.emplace(handle, targets.size());
			if (inserted) {
				auto state = detail::findSocket(handle);
				if (!state)
					return -1;
				targets.push_back({handle, std::move(state)});
			}
			set.targets.push_back(found->second);
		}
	}
	if (targets.empty())
		return detail::failSocket(10022);
	std::vector<pollfd> descriptors(targets.size());
	for (size_t index = 0; index != targets.size(); ++index)
		descriptors[index].fd = targets[index].socket->descriptor;
	for (const auto index : sets[0].targets)
		descriptors[index].events |= POLLIN;
	for (const auto index : sets[1].targets)
		descriptors[index].events |= POLLOUT;
	for (const auto index : sets[2].targets)
		descriptors[index].events |= POLLPRI;
	bool firstPoll = true;
	while (true) {
		int milliseconds = -1;
		if (timeout) {
			const auto remaining = deadline - std::chrono::steady_clock::now();
			if (!firstPoll && remaining <= decltype(remaining)::zero())
				break;
			const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
			milliseconds = static_cast<int>(std::clamp<int64_t>(rounded, 0, INT_MAX));
		}
		firstPoll = false;
		const int status = waitForSockets(descriptors, targets, milliseconds);
		if (status < 0) {
			if (errno == EINTR)
				continue;
			return detail::failSocket(detail::socketError(errno));
		}
		if (!status) {
			if (!timeout || std::chrono::steady_clock::now() < deadline)
				continue;
			break;
		}
		bool ready = false;
		for (size_t index = 0; index != targets.size(); ++index) {
			auto &target = targets[index];
			auto &descriptor = descriptors[index];
			if (descriptor.revents & POLLNVAL)
				return detail::failSocket(10038);
			if (target.socket->connecting.load() && (descriptor.revents & (POLLIN | POLLOUT | POLLHUP)) &&
				!(descriptor.revents & POLLERR)) {
				sockaddr_storage peer{};
				socklen_t size = sizeof(peer);
				if (::getpeername(target.socket->descriptor, reinterpret_cast<sockaddr *>(&peer), &size) == 0)
					target.socket->connecting.store(false);
			}
			const bool failedConnection =
				target.socket->connecting.load() && (descriptor.revents & (POLLERR | POLLHUP));
			target.read = !failedConnection && (descriptor.revents & (POLLIN | POLLERR | POLLHUP));
			target.write = !failedConnection && (descriptor.revents & POLLOUT);
			// Some hosts report priority readiness on a closed stream even without urgent data.
			char urgent = 0;
			const bool outOfBand = (descriptor.revents & POLLPRI) && ::recv(target.socket->descriptor, &urgent, 1,
																			MSG_OOB | MSG_PEEK | MSG_DONTWAIT) == 1;
			target.exception = failedConnection || outOfBand;
			const bool descriptorReady = ((descriptor.events & POLLIN) && target.read) ||
										 ((descriptor.events & POLLOUT) && target.write) ||
										 ((descriptor.events & POLLPRI) && target.exception);
			ready |= descriptorReady;
			if (!descriptorReady && (descriptor.revents & (POLLERR | POLLHUP)))
				descriptor.fd = -1;
		}
		if (ready)
			break;
	}
	int total = 0;
	for (size_t kind = 0; kind != sets.size(); ++kind) {
		auto &set = sets[kind];
		if (!set.output)
			continue;
		DWORD count = 0;
		auto *bytes = static_cast<uint8_t *>(set.output) + offsetof(WSA_FD_SET, sockets);
		for (const auto index : set.targets) {
			const auto &target = targets[index];
			const bool ready = kind == 0 ? target.read : kind == 1 ? target.write : target.exception;
			if (ready) {
				std::memcpy(bytes + count++ * sizeof(SOCKET), &target.handle, sizeof(SOCKET));
				++total;
			}
		}
		std::memcpy(set.output, &count, sizeof(count));
	}
	return total;
}
} // namespace ws2
