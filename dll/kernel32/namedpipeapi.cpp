#include "namedpipeapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "overlapped_util.h"
#include "strutil.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace kernel32 {

namespace {

struct NamedPipeInstance;

constexpr uint32_t kTransferMagic = 0x50495045;
constexpr size_t kMaxTransferName = 256;
constexpr size_t kMaxTransferClients = 64;

struct PipeTransferRequest {
	uint32_t magic;
	uint32_t access;
	uint32_t nameLength;
};

struct PipeTransferReply {
	uint32_t magic;
	uint32_t error;
	uint32_t pipeMode;
	uint32_t accessMode;
};
static_assert(sizeof(PipeTransferRequest) == 12);
static_assert(sizeof(PipeTransferReply) == 16);

std::string pipeSocketPath(std::string_view key) {
	uint64_t hash = 14695981039346656037ULL;
	for (unsigned char character : key) {
		hash ^= character;
		hash *= 1099511628211ULL;
	}
	char path[96];
	std::snprintf(path, sizeof(path), "/tmp/wibo-pipes-%lu/%016llx", static_cast<unsigned long>(geteuid()),
				  static_cast<unsigned long long>(hash));
	return path;
}

bool ensurePipeSocketDirectory() {
	const std::string path = pipeSocketPath("");
	const auto separator = path.find_last_of('/');
	const std::string directory = path.substr(0, separator);
	if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
		return false;
	struct stat info{};
	return lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
		   (info.st_mode & 077) == 0;
}

bool makePipeSocketAddress(std::string_view key, sockaddr_un &address, socklen_t &length) {
	const std::string path = pipeSocketPath(key);
	if (path.size() >= sizeof(address.sun_path))
		return false;
	address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
	length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
	return true;
}

bool receiveBytes(int fd, void *buffer, size_t length) {
	auto *bytes = static_cast<char *>(buffer);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (length != 0) {
		const auto remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0)
			return false;
		pollfd descriptor{fd, POLLIN, 0};
		const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			return false;
		ssize_t received;
		do {
			received = recv(fd, bytes, length, 0);
		} while (received < 0 && errno == EINTR);
		if (received <= 0)
			return false;
		bytes += received;
		length -= static_cast<size_t>(received);
	}
	return true;
}

bool sendBytes(int fd, const void *buffer, size_t length) {
	const auto *bytes = static_cast<const char *>(buffer);
	while (length != 0) {
#ifdef MSG_NOSIGNAL
		constexpr int flags = MSG_NOSIGNAL;
#else
		constexpr int flags = 0;
#endif
		ssize_t sent;
		do {
			sent = send(fd, bytes, length, flags);
		} while (sent < 0 && errno == EINTR);
		if (sent <= 0)
			return false;
		bytes += sent;
		length -= static_cast<size_t>(sent);
	}
	return true;
}

bool sendReply(int socketFd, const PipeTransferReply &reply, int transferredFd) {
	iovec data{const_cast<PipeTransferReply *>(&reply), sizeof(reply)};
	msghdr message{};
	message.msg_iov = &data;
	message.msg_iovlen = 1;
	std::array<char, CMSG_SPACE(sizeof(int))> control{};
	if (transferredFd >= 0) {
		message.msg_control = control.data();
		message.msg_controllen = control.size();
		cmsghdr *header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(header), &transferredFd, sizeof(transferredFd));
	}
#ifdef MSG_NOSIGNAL
	constexpr int flags = MSG_NOSIGNAL;
#else
	constexpr int flags = 0;
#endif
	ssize_t sent;
	do {
		sent = sendmsg(socketFd, &message, flags);
	} while (sent < 0 && errno == EINTR);
	return sent > 0 && (static_cast<size_t>(sent) == sizeof(reply) ||
						sendBytes(socketFd, reinterpret_cast<const char *>(&reply) + sent, sizeof(reply) - sent));
}

bool receiveReply(int socketFd, PipeTransferReply &reply, int &transferredFd) {
	transferredFd = -1;
	iovec data{&reply, sizeof(reply)};
	std::array<char, CMSG_SPACE(sizeof(int))> control{};
	msghdr message{};
	message.msg_iov = &data;
	message.msg_iovlen = 1;
	message.msg_control = control.data();
	message.msg_controllen = control.size();
	ssize_t received;
	do {
		received = recvmsg(socketFd, &message, 0);
	} while (received < 0 && errno == EINTR);
	if (received <= 0)
		return false;
	if (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
		return false;
	for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
			header->cmsg_len >= CMSG_LEN(sizeof(int))) {
			std::memcpy(&transferredFd, CMSG_DATA(header), sizeof(transferredFd));
			break;
		}
	}
	return static_cast<size_t>(received) == sizeof(reply) ||
		   receiveBytes(socketFd, reinterpret_cast<char *>(&reply) + received, sizeof(reply) - received);
}

void configureInheritability(int fd, bool inherit) {
	if (fd < 0) {
		return;
	}
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		return;
	}
	if (inherit) {
		flags &= ~FD_CLOEXEC;
	} else {
		flags |= FD_CLOEXEC;
	}
	fcntl(fd, F_SETFD, flags);
}

struct ParsedPipeName {
	std::string key;
	std::u16string namespaceKey;
};

std::optional<ParsedPipeName> parsePipeName(LPCSTR name, DWORD &error) {
	error = ERROR_SUCCESS;
	if (!name) {
		error = ERROR_PATH_NOT_FOUND;
		return std::nullopt;
	}
	std::string_view input{name};
	if (input.empty()) {
		error = ERROR_INVALID_NAME;
		return std::nullopt;
	}
	std::string lower;
	lower.reserve(input.size());
	for (unsigned char ch : input) {
		lower.push_back(ch < 0x80 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch));
	}
	constexpr std::string_view kLocalPrefix = "\\\\.\\pipe\\";
	constexpr std::string_view kNtPrefix = "\\\\?\\pipe\\";
	size_t prefixLen = 0;
	if (lower.rfind(kLocalPrefix, 0) == 0) {
		prefixLen = kLocalPrefix.size();
	} else if (lower.rfind(kNtPrefix, 0) == 0) {
		prefixLen = kNtPrefix.size();
	} else {
		// Not a pipe path; treat as non-match without error.
		return std::nullopt;
	}
	if (input.size() > 256) {
		error = ERROR_INVALID_PARAMETER;
		return std::nullopt;
	}
	std::string raw = std::string(input.substr(prefixLen));
	if (raw.empty()) {
		error = ERROR_INVALID_HANDLE;
		return std::nullopt;
	}
	if (raw.find('\\') != std::string::npos || raw.find('/') != std::string::npos) {
		error = ERROR_INVALID_NAME;
		return std::nullopt;
	}
	std::string key = lower.substr(prefixLen);
	return ParsedPipeName{std::move(key), stringToUtf16(lower.substr(prefixLen))};
}

DWORD normalizeMaxInstances(DWORD value) {
	if (value == 0) {
		return 1;
	}
	if (value >= PIPE_UNLIMITED_INSTANCES) {
		return PIPE_UNLIMITED_INSTANCES;
	}
	return value;
}

struct NamedPipeState : ObjectBase {
	static constexpr ObjectType kType = ObjectType::NamedPipeState;

	std::mutex mutex;
	std::string key;
	DWORD accessMode = PIPE_ACCESS_DUPLEX;
	DWORD pipeType = PIPE_TYPE_BYTE;
	DWORD defaultTimeout = 0;
	DWORD maxInstances = PIPE_UNLIMITED_INSTANCES;
	uint32_t instanceCount = 0;
	std::vector<NamedPipeInstance *> instances;
	std::mutex listenerMutex;
	int listenerFd = -1;
	std::string listenerPath;
	std::thread listenerThread;
	std::atomic<bool> listenerStopping{false};

	explicit NamedPipeState(std::string k) : ObjectBase(kType), key(std::move(k)) {}
	~NamedPipeState() override {
		listenerStopping.store(true, std::memory_order_release);
		if (listenerThread.joinable())
			listenerThread.join();
		if (listenerFd >= 0)
			close(listenerFd);
		if (!listenerPath.empty())
			unlink(listenerPath.c_str());
		wibo::g_namespace.remove(this);
	}

	bool ensureListener(DWORD &error);
	void listenForClients();
	void serveClient(int socketFd);

	void registerInstance(NamedPipeInstance *inst) {
		std::lock_guard lk(mutex);
		instances.push_back(inst);
	}

	void unregisterInstance(NamedPipeInstance *inst) {
		std::lock_guard lk(mutex);
		auto it = std::find(instances.begin(), instances.end(), inst);
		if (it != instances.end()) {
			instances.erase(it);
		}
	}

	bool reserveInstance(DWORD access, DWORD type, DWORD timeout, DWORD maxAllowed, bool firstFlag, bool isNew,
						 DWORD &error) {
		error = ERROR_SUCCESS;
		std::lock_guard lk(mutex);
		if (isNew) {
			accessMode = access;
			pipeType = type;
			defaultTimeout = timeout;
			maxInstances = maxAllowed;
		} else {
			if (accessMode != access || pipeType != type || defaultTimeout != timeout) {
				error = ERROR_ACCESS_DENIED;
				return false;
			}
			if (maxInstances != maxAllowed) {
				error = ERROR_ACCESS_DENIED;
				return false;
			}
		}
		if (firstFlag && instanceCount > 0) {
			error = ERROR_ACCESS_DENIED;
			return false;
		}
		if (maxInstances != PIPE_UNLIMITED_INSTANCES && instanceCount >= maxInstances) {
			error = ERROR_PIPE_BUSY;
			return false;
		}
		++instanceCount;
		return true;
	}

	bool releaseInstance() {
		std::lock_guard lk(mutex);
		if (instanceCount > 0) {
			--instanceCount;
		}
		return instanceCount == 0;
	}
};

struct NamedPipeInstance final : FileObject {
	static constexpr ObjectType kType = ObjectType::NamedPipe;

	Pin<NamedPipeState> state;
	int companionFd = -1;
	DWORD accessMode;
	DWORD pipeMode;
	bool clientConnected = false;
	bool requiresConnect = false;
	bool connectPending = false;
	LPOVERLAPPED pendingOverlapped = nullptr;
	pthread_t pendingThread{};
	std::mutex connectMutex;
	std::condition_variable connectCv;

	NamedPipeInstance(int fd, Pin<NamedPipeState> st, int companion, DWORD open, DWORD mode)
		: FileObject(kType, fd), state(std::move(st)), companionFd(companion), accessMode(open), pipeMode(mode) {
		pipeMessageMode = (mode & PIPE_TYPE_MESSAGE) != 0;
		if (state) {
			state->registerInstance(this);
		}
	}

	~NamedPipeInstance() override {
		int localCompanion = -1;
		{
			std::lock_guard lk(connectMutex);
			localCompanion = companionFd;
			companionFd = -1;
			kernel32::detail::signalOverlappedEvent(this, pendingOverlapped, STATUS_PIPE_BROKEN, 0);
			pendingOverlapped = nullptr;
			connectPending = false;
			connectCv.notify_all();
		}
		if (localCompanion >= 0) {
			close(localCompanion);
		}
		if (state) {
			state->unregisterInstance(this);
			state->releaseInstance();
		}
	}

	bool canAcceptClient(DWORD desiredAccess) {
		std::lock_guard lk(connectMutex);
		if (companionFd < 0 || clientConnected || requiresConnect) {
			return false;
		}
		DWORD access = accessMode & PIPE_ACCESS_DUPLEX;
		switch (access) {
		case PIPE_ACCESS_DUPLEX:
			return (desiredAccess & (GENERIC_READ | GENERIC_WRITE)) != 0;
		case PIPE_ACCESS_INBOUND:
			return (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
		case PIPE_ACCESS_OUTBOUND:
			return (desiredAccess & (GENERIC_READ | FILE_READ_DATA)) != 0;
		default:
			return false;
		}
	}

	int takeCompanion() {
		std::lock_guard lk(connectMutex);
		if (companionFd < 0 || clientConnected || requiresConnect) {
			return -1;
		}
		int fd = companionFd;
		companionFd = -1;
		clientConnected = true;
		kernel32::detail::signalOverlappedEvent(this, pendingOverlapped, STATUS_SUCCESS, 0);
		pendingOverlapped = nullptr;
		if (connectPending) {
			connectPending = false;
			connectCv.notify_all();
		}
		return fd;
	}

	void restoreCompanion(int fd) {
		std::lock_guard lk(connectMutex);
		companionFd = fd;
		if (fd >= 0) {
			clientConnected = false;
		}
	}
};

bool NamedPipeState::ensureListener(DWORD &error) {
	std::lock_guard lock(listenerMutex);
	if (listenerFd >= 0)
		return true;
	if (!ensurePipeSocketDirectory()) {
		error = ERROR_ACCESS_DENIED;
		return false;
	}
	sockaddr_un address{};
	socklen_t addressLength = 0;
	if (!makePipeSocketAddress(key, address, addressLength)) {
		error = ERROR_INVALID_NAME;
		return false;
	}
	const int candidate = socket(AF_UNIX, SOCK_STREAM, 0);
	if (candidate < 0) {
		error = wibo::winErrorFromErrno(errno);
		return false;
	}
	configureInheritability(candidate, false);
	if (bind(candidate, reinterpret_cast<const sockaddr *>(&address), addressLength) != 0) {
		const int bindError = errno;
		if (bindError != EADDRINUSE) {
			error = wibo::winErrorFromErrno(bindError);
			close(candidate);
			return false;
		}
		struct stat existing{};
		if (lstat(address.sun_path, &existing) != 0 || !S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
			error = ERROR_ACCESS_DENIED;
			close(candidate);
			return false;
		}
		const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
		int probeError = EADDRINUSE;
		if (probe >= 0) {
			if (connect(probe, reinterpret_cast<const sockaddr *>(&address), addressLength) != 0)
				probeError = errno;
			close(probe);
		}
		if (probeError != ECONNREFUSED || unlink(address.sun_path) != 0 ||
			bind(candidate, reinterpret_cast<const sockaddr *>(&address), addressLength) != 0) {
			error = ERROR_ACCESS_DENIED;
			close(candidate);
			return false;
		}
	}
	if (chmod(address.sun_path, 0600) != 0 || listen(candidate, 64) != 0) {
		error = wibo::winErrorFromErrno(errno);
		unlink(address.sun_path);
		close(candidate);
		return false;
	}
	listenerFd = candidate;
	listenerPath = address.sun_path;
	listenerThread = std::thread(&NamedPipeState::listenForClients, this);
	return true;
}

void NamedPipeState::listenForClients() {
	std::vector<std::future<void>> clients;
	clients.reserve(kMaxTransferClients);
	while (!listenerStopping.load(std::memory_order_acquire)) {
		pollfd descriptor{listenerFd, POLLIN, 0};
		int ready;
		do {
			ready = poll(&descriptor, 1, 100);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0 || !(descriptor.revents & POLLIN))
			continue;
		int socketFd;
		do {
			socketFd = accept(listenerFd, nullptr, nullptr);
		} while (socketFd < 0 && errno == EINTR);
		if (socketFd < 0)
			continue;
		configureInheritability(socketFd, false);
		clients.erase(std::remove_if(clients.begin(), clients.end(),
									 [](std::future<void> &client) {
										 return client.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
									 }),
					  clients.end());
		if (clients.size() >= kMaxTransferClients) {
			close(socketFd);
			continue;
		}
		clients.emplace_back(std::async(std::launch::async, [this, socketFd] {
			serveClient(socketFd);
			close(socketFd);
		}));
	}
}

void NamedPipeState::serveClient(int socketFd) {
	PipeTransferRequest request{};
	if (!receiveBytes(socketFd, &request, sizeof(request)) || request.magic != kTransferMagic ||
		request.nameLength == 0 || request.nameLength > kMaxTransferName)
		return;
	std::string requestedName(request.nameLength, '\0');
	if (!receiveBytes(socketFd, requestedName.data(), requestedName.size()))
		return;
	PipeTransferReply reply{kTransferMagic, ERROR_PIPE_BUSY, 0, 0};
	int companion = -1;
	if (requestedName != key) {
		reply.error = ERROR_ACCESS_DENIED;
	} else {
		std::lock_guard lock(mutex);
		for (auto *instance : instances) {
			if (!instance || !instance->canAcceptClient(request.access))
				continue;
			companion = instance->takeCompanion();
			if (companion >= 0) {
				reply.error = ERROR_SUCCESS;
				reply.pipeMode = instance->pipeMode;
				reply.accessMode = instance->accessMode;
				break;
			}
		}
	}
	const bool delivered = sendReply(socketFd, reply, companion);
	if (delivered && companion >= 0) {
		// Retain the source descriptor until the receiver installs its copy.
		char acknowledged = 0;
		receiveBytes(socketFd, &acknowledged, sizeof(acknowledged));
	}
	if (companion >= 0)
		close(companion);
}

Pin<NamedPipeInstance> acquireConnectableInstance(Pin<NamedPipeState> &state, DWORD desiredAccess, DWORD &error) {
	if (!state) {
		error = ERROR_FILE_NOT_FOUND;
		return {};
	}
	std::lock_guard lk(state->mutex);
	for (auto *inst : state->instances) {
		if (!inst) {
			continue;
		}
		if (inst->canAcceptClient(desiredAccess)) {
			return Pin<NamedPipeInstance>::acquire(inst);
		}
	}
	error = ERROR_PIPE_BUSY;
	return {};
}

struct RemotePipeEndpoint {
	int fd = -1;
	DWORD pipeMode = 0;
	DWORD accessMode = 0;
};

std::optional<RemotePipeEndpoint> connectRemotePipe(const ParsedPipeName &name, DWORD desiredAccess, DWORD &error) {
	sockaddr_un address{};
	socklen_t addressLength = 0;
	if (!makePipeSocketAddress(name.key, address, addressLength)) {
		error = ERROR_INVALID_NAME;
		return std::nullopt;
	}
	const int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socketFd < 0) {
		error = wibo::winErrorFromErrno(errno);
		return std::nullopt;
	}
	configureInheritability(socketFd, false);
	if (connect(socketFd, reinterpret_cast<const sockaddr *>(&address), addressLength) != 0) {
		const int failure = errno;
		close(socketFd);
		error = failure == ENOENT || failure == ECONNREFUSED ? ERROR_FILE_NOT_FOUND : wibo::winErrorFromErrno(failure);
		return std::nullopt;
	}
	PipeTransferRequest request{kTransferMagic, desiredAccess, static_cast<uint32_t>(name.key.size())};
	PipeTransferReply reply{};
	int transferred = -1;
	pollfd descriptor{socketFd, POLLIN, 0};
	int ready;
	if (!sendBytes(socketFd, &request, sizeof(request)) || !sendBytes(socketFd, name.key.data(), name.key.size())) {
		close(socketFd);
		error = ERROR_PIPE_NOT_CONNECTED;
		return std::nullopt;
	}
	do {
		ready = poll(&descriptor, 1, 3000);
	} while (ready < 0 && errno == EINTR);
	const bool received = ready > 0 && receiveReply(socketFd, reply, transferred);
	if (received && reply.magic == kTransferMagic && reply.error == ERROR_SUCCESS && transferred >= 0) {
		const char acknowledged = 1;
		if (!sendBytes(socketFd, &acknowledged, sizeof(acknowledged))) {
			close(socketFd);
			close(transferred);
			error = ERROR_PIPE_NOT_CONNECTED;
			return std::nullopt;
		}
	}
	close(socketFd);
	if (!received || reply.magic != kTransferMagic || (reply.error == ERROR_SUCCESS && transferred < 0)) {
		if (transferred >= 0)
			close(transferred);
		error = ERROR_PIPE_NOT_CONNECTED;
		return std::nullopt;
	}
	if (reply.error != ERROR_SUCCESS) {
		if (transferred >= 0)
			close(transferred);
		error = reply.error;
		return std::nullopt;
	}
	return RemotePipeEndpoint{transferred, reply.pipeMode, reply.accessMode};
}

struct PipePeek {
	DWORD available = 0;
	DWORD copied = 0;
	bool closed = false;
};

// The caller holds the file lock so availability and data describe the same peek.
DWORD peekPipeBytes(FileObject *pipe, void *buffer, DWORD capacity, PipePeek &result) {
	int available = 0;
	if (ioctl(pipe->fd, FIONREAD, &available) != 0)
		return wibo::winErrorFromErrno(errno);
	result.available = static_cast<DWORD>(std::max(available, 0));
	pollfd descriptor{pipe->fd, POLLIN, 0};
	int ready;
	do {
		ready = poll(&descriptor, 1, 0);
	} while (ready < 0 && errno == EINTR);
	if (ready < 0)
		return wibo::winErrorFromErrno(errno);
	result.closed = (descriptor.revents & POLLHUP) != 0;
	if (buffer && capacity && result.available) {
		// Socketpairs support a non-consuming peek; Unix FIFOs cannot peek payloads.
		const size_t requested = std::min<size_t>({capacity, result.available, SSIZE_MAX});
		ssize_t count;
		do {
			count = recv(pipe->fd, buffer, requested, MSG_PEEK | MSG_DONTWAIT);
		} while (count < 0 && errno == EINTR);
		if (count < 0)
			return errno == ENOTSOCK ? ERROR_NOT_SUPPORTED : wibo::winErrorFromErrno(errno);
		result.copied = static_cast<DWORD>(count);
	}
	return ERROR_SUCCESS;
}

} // namespace

NTSTATUS peekPipeControl(FileObject *pipe, void *output, ULONG length, ULONG_PTR &information) {
	constexpr ULONG kHeaderSize = 4 * sizeof(ULONG);
	if (length < kHeaderSize)
		return STATUS_INFO_LENGTH_MISMATCH;
	if (!output)
		return STATUS_ACCESS_VIOLATION;
	if (pipe->pipeMessageMode)
		return STATUS_NOT_SUPPORTED;
	if (auto *instance = ::detail::castTo<NamedPipeInstance>(pipe)) {
		std::lock_guard connectLock(instance->connectMutex);
		if (!instance->clientConnected)
			return STATUS_INVALID_PIPE_STATE;
	}
	std::lock_guard lock(pipe->m);
	PipePeek result;
	const DWORD error = peekPipeBytes(pipe, static_cast<BYTE *>(output) + kHeaderSize, length - kHeaderSize, result);
	if (error)
		return error == ERROR_NOT_SUPPORTED ? STATUS_NOT_SUPPORTED : wibo::statusFromWinError(error);
	if (result.closed && !result.available)
		return STATUS_PIPE_BROKEN;
	const ULONG header[] = {result.closed ? 4U : 3U, result.available, 0, 0};
	memcpy(output, header, sizeof(header));
	information = kHeaderSize + result.copied;
	return STATUS_SUCCESS;
}

bool tryCreateFileNamedPipeA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
							 LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
							 DWORD dwFlagsAndAttributes, HANDLE &outHandle) {
	(void)dwShareMode;
	(void)dwCreationDisposition;

	DWORD parseError = ERROR_SUCCESS;
	auto parsed = parsePipeName(lpFileName, parseError);
	if (!parsed) {
		if (parseError != ERROR_SUCCESS) {
			setLastError(parseError);
			outHandle = INVALID_HANDLE_VALUE;
			return true;
		}
		return false;
	}

	auto state = wibo::g_namespace.getAs<NamedPipeState>(parsed->namespaceKey);
	if (!state) {
		DWORD remoteError = ERROR_SUCCESS;
		auto remote = connectRemotePipe(*parsed, dwDesiredAccess, remoteError);
		if (!remote) {
			setLastError(remoteError);
			outHandle = INVALID_HANDLE_VALUE;
			return true;
		}
		const bool inherit = lpSecurityAttributes && lpSecurityAttributes->bInheritHandle;
		configureInheritability(remote->fd, inherit);
		auto client = make_pin<FileObject>(remote->fd);
		if (!client) {
			close(remote->fd);
			setLastError(ERROR_NOT_ENOUGH_MEMORY);
			outHandle = INVALID_HANDLE_VALUE;
			return true;
		}
		client->pipeMessageMode = (remote->pipeMode & PIPE_TYPE_MESSAGE) != 0;
		client->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
		client->overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;
		uint32_t grantedAccess = SYNCHRONIZE;
		switch (remote->accessMode & PIPE_ACCESS_DUPLEX) {
		case PIPE_ACCESS_DUPLEX:
			grantedAccess |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
			break;
		case PIPE_ACCESS_INBOUND:
			grantedAccess |= FILE_GENERIC_WRITE;
			break;
		case PIPE_ACCESS_OUTBOUND:
			grantedAccess |= FILE_GENERIC_READ;
			break;
		default:
			break;
		}
		outHandle = wibo::handles().alloc(std::move(client), grantedAccess, inherit ? HANDLE_FLAG_INHERIT : 0);
		return true;
	}

	DWORD acquireError = ERROR_SUCCESS;
	auto instancePin = acquireConnectableInstance(state, dwDesiredAccess, acquireError);
	if (!instancePin) {
		setLastError(acquireError);
		outHandle = INVALID_HANDLE_VALUE;
		return true;
	}

	int clientFd = instancePin->takeCompanion();
	if (clientFd < 0) {
		setLastError(ERROR_PIPE_BUSY);
		outHandle = INVALID_HANDLE_VALUE;
		return true;
	}

	bool inherit = lpSecurityAttributes && lpSecurityAttributes->bInheritHandle;
	configureInheritability(clientFd, inherit);

	auto clientObj = make_pin<FileObject>(clientFd);
	if (!clientObj) {
		instancePin->restoreCompanion(clientFd);
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		outHandle = INVALID_HANDLE_VALUE;
		return true;
	}
	clientFd = -1;

	clientObj->pipeMessageMode = instancePin->pipeMessageMode;
	clientObj->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	clientObj->overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;

	uint32_t grantedAccess = SYNCHRONIZE;
	switch (instancePin->accessMode & PIPE_ACCESS_DUPLEX) {
	case PIPE_ACCESS_DUPLEX:
		grantedAccess |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_INBOUND:
		grantedAccess |= FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_OUTBOUND:
		grantedAccess |= FILE_GENERIC_READ;
		break;
	default:
		break;
	}

	uint32_t handleFlags = inherit ? HANDLE_FLAG_INHERIT : 0;
	outHandle = wibo::handles().alloc(std::move(clientObj), grantedAccess, handleFlags);
	return true;
}

BOOL WINAPI CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe, LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreatePipe(%p, %p, %p, %u)\n", hReadPipe, hWritePipe, lpPipeAttributes, nSize);
	if (!hReadPipe || !hWritePipe) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*hReadPipe = NO_HANDLE;
	*hWritePipe = NO_HANDLE;

	int pipeFds[2];
	if (pipe(pipeFds) != 0) {
		setLastErrorFromErrno();
		return FALSE;
	}

	bool inheritHandles = lpPipeAttributes && lpPipeAttributes->bInheritHandle;
	configureInheritability(pipeFds[0], inheritHandles);
	configureInheritability(pipeFds[1], inheritHandles);

#ifdef __linux__
	if (nSize != 0) {
		// Best-effort adjustment; ignore failures as recommended by docs.
		fcntl(pipeFds[0], F_SETPIPE_SZ, static_cast<int>(nSize));
		fcntl(pipeFds[1], F_SETPIPE_SZ, static_cast<int>(nSize));
	}
#endif

	auto readObj = make_pin<FileObject>(pipeFds[0]);
	readObj->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	auto writeObj = make_pin<FileObject>(pipeFds[1]);
	writeObj->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	*hReadPipe = wibo::handles().alloc(std::move(readObj), FILE_GENERIC_READ, inheritHandles ? HANDLE_FLAG_INHERIT : 0);
	*hWritePipe =
		wibo::handles().alloc(std::move(writeObj), FILE_GENERIC_WRITE, inheritHandles ? HANDLE_FLAG_INHERIT : 0);
	return TRUE;
}

BOOL WINAPI PeekNamedPipe(HANDLE hNamedPipe, LPVOID lpBuffer, DWORD nBufferSize, LPDWORD lpBytesRead,
						  LPDWORD lpTotalBytesAvail, LPDWORD lpBytesLeftThisMessage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PeekNamedPipe(%p, %p, %u, %p, %p, %p)\n", hNamedPipe, lpBuffer, nBufferSize, lpBytesRead,
			  lpTotalBytesAvail, lpBytesLeftThisMessage);

	if (lpBytesRead) {
		*lpBytesRead = 0;
	}
	if (lpTotalBytesAvail) {
		*lpTotalBytesAvail = 0;
	}
	if (lpBytesLeftThisMessage) {
		*lpBytesLeftThisMessage = 0;
	}
	if (nBufferSize != 0 && !lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	HandleMeta meta{};
	auto pipe = wibo::handles().getAs<FileObject>(hNamedPipe, &meta);
	if (!pipe || !pipe->valid() || !pipe->isPipe) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if ((meta.grantedAccess & FILE_READ_DATA) == 0) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}

	std::lock_guard lock(pipe->m);
	PipePeek result;
	const DWORD error = peekPipeBytes(pipe.get(), lpBuffer, nBufferSize, result);
	if (error) {
		setLastError(error);
		return FALSE;
	}
	if (lpBytesRead)
		*lpBytesRead = result.copied;
	if (lpTotalBytesAvail)
		*lpTotalBytesAvail = result.available;
	// Wibo's named-pipe transport is byte-stream based. There is therefore no
	// remainder in a discrete message to report.
	if (lpBytesLeftThisMessage) {
		*lpBytesLeftThisMessage = 0;
	}
	return TRUE;
}

HANDLE WINAPI CreateNamedPipeA(LPCSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
							   DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
							   LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateNamedPipeA(%s, 0x%08x, 0x%08x, %u, %u, %u, %u, %p)\n", lpName ? lpName : "(null)", dwOpenMode,
			  dwPipeMode, nMaxInstances, nOutBufferSize, nInBufferSize, nDefaultTimeOut, lpSecurityAttributes);

	DWORD parseError = ERROR_SUCCESS;
	std::optional<ParsedPipeName> parsed = parsePipeName(lpName, parseError);
	if (!parsed) {
		setLastError((parseError == ERROR_SUCCESS) ? ERROR_INVALID_NAME : parseError);
		return INVALID_HANDLE_VALUE;
	}

	constexpr DWORD kAllowedOpenFlags = PIPE_ACCESS_DUPLEX | WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY |
										FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED;
	if ((dwOpenMode & ~kAllowedOpenFlags) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	DWORD accessMode = dwOpenMode & PIPE_ACCESS_DUPLEX;
	if (accessMode != PIPE_ACCESS_DUPLEX && accessMode != PIPE_ACCESS_INBOUND && accessMode != PIPE_ACCESS_OUTBOUND) {
		setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	const bool firstInstanceFlag = (dwOpenMode & FILE_FLAG_FIRST_PIPE_INSTANCE) != 0;
	const bool inheritHandles = lpSecurityAttributes && lpSecurityAttributes->bInheritHandle;
	const bool overlapped = (dwOpenMode & FILE_FLAG_OVERLAPPED) != 0;

	constexpr DWORD kAllowedPipeModeFlags =
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS;
	if ((dwPipeMode & ~kAllowedPipeModeFlags) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if ((dwPipeMode & PIPE_READMODE_MESSAGE) != 0 && (dwPipeMode & PIPE_TYPE_MESSAGE) == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	DWORD pipeType = (dwPipeMode & PIPE_TYPE_MESSAGE) != 0 ? PIPE_TYPE_MESSAGE : PIPE_TYPE_BYTE;
	DWORD normalizedMaxInstances = normalizeMaxInstances(nMaxInstances);

	auto [state, isNewState] = wibo::g_namespace.getOrCreate(
		parsed->namespaceKey, [&]() -> NamedPipeState * { return new NamedPipeState(parsed->key); });
	if (!state) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}

	bool instanceReserved = false;
	DWORD reserveError = ERROR_SUCCESS;
	if (!state->reserveInstance(accessMode, pipeType, nDefaultTimeOut, normalizedMaxInstances, firstInstanceFlag,
								isNewState, reserveError)) {
		setLastError(reserveError);
		return INVALID_HANDLE_VALUE;
	}
	instanceReserved = true;

	int serverFd = -1;
	int companionFd = -1;
	auto fail = [&](DWORD err) -> HANDLE {
		if (serverFd >= 0) {
			close(serverFd);
			serverFd = -1;
		}
		if (companionFd >= 0) {
			close(companionFd);
			companionFd = -1;
		}
		if (instanceReserved && state) {
			state->releaseInstance();
			instanceReserved = false;
		}
		setLastError(err);
		return INVALID_HANDLE_VALUE;
	};

	int fds[2] = {-1, -1};
	if (accessMode == PIPE_ACCESS_DUPLEX) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
			int savedErrno = errno;
			return fail(wibo::winErrorFromErrno(savedErrno));
		}
		serverFd = fds[0];
		companionFd = fds[1];
	} else {
		if (pipe(fds) != 0) {
			int savedErrno = errno;
			return fail(wibo::winErrorFromErrno(savedErrno));
		}
		if (accessMode == PIPE_ACCESS_INBOUND) {
			serverFd = fds[0];
			companionFd = fds[1];
#ifdef __linux__
			if (nInBufferSize != 0) {
				fcntl(serverFd, F_SETPIPE_SZ, static_cast<int>(nInBufferSize));
			}
#endif
		} else {
			serverFd = fds[1];
			companionFd = fds[0];
#ifdef __linux__
			if (nOutBufferSize != 0) {
				fcntl(serverFd, F_SETPIPE_SZ, static_cast<int>(nOutBufferSize));
			}
#endif
		}
	}

	configureInheritability(serverFd, inheritHandles);
	if (companionFd >= 0) {
		configureInheritability(companionFd, inheritHandles);
	}

	auto pipeObj = make_pin<NamedPipeInstance>(serverFd, std::move(state), companionFd, accessMode, dwPipeMode);
	if (!pipeObj) {
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	}
	serverFd = -1;
	companionFd = -1;
	instanceReserved = false;

	pipeObj->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	pipeObj->overlapped = overlapped;
	DWORD listenerError = ERROR_SUCCESS;
	if (!pipeObj->state->ensureListener(listenerError)) {
		setLastError(listenerError);
		return INVALID_HANDLE_VALUE;
	}

	uint32_t grantedAccess = SYNCHRONIZE;
	switch (accessMode) {
	case PIPE_ACCESS_DUPLEX:
		grantedAccess |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_INBOUND:
		grantedAccess |= FILE_GENERIC_READ;
		break;
	case PIPE_ACCESS_OUTBOUND:
		grantedAccess |= FILE_GENERIC_WRITE;
		break;
	default:
		break;
	}

	uint32_t handleFlags = inheritHandles ? HANDLE_FLAG_INHERIT : 0;
	return wibo::handles().alloc(std::move(pipeObj), grantedAccess, handleFlags);
}

HANDLE WINAPI CreateNamedPipeW(LPCWSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
							   DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
							   LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateNamedPipeW(%p, 0x%08x, 0x%08x, %u, %u, %u, %u, %p)\n", lpName, dwOpenMode, dwPipeMode,
			  nMaxInstances, nOutBufferSize, nInBufferSize, nDefaultTimeOut, lpSecurityAttributes);
	if (!lpName)
		return CreateNamedPipeA(nullptr, dwOpenMode, dwPipeMode, nMaxInstances, nOutBufferSize, nInBufferSize,
								nDefaultTimeOut, lpSecurityAttributes);
	std::string name;
	if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(lpName), wstrlen(lpName)), name)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return INVALID_HANDLE_VALUE;
	}
	return CreateNamedPipeA(name.c_str(), dwOpenMode, dwPipeMode, nMaxInstances, nOutBufferSize, nInBufferSize,
							nDefaultTimeOut, lpSecurityAttributes);
}

BOOL WINAPI ConnectNamedPipe(HANDLE hNamedPipe, LPOVERLAPPED lpOverlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ConnectNamedPipe(%p, %p)\n", hNamedPipe, lpOverlapped);

	auto pipe = wibo::handles().getAs<NamedPipeInstance>(hNamedPipe);
	if (!pipe) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	const bool isOverlappedHandle = pipe->overlapped;
	if (isOverlappedHandle && lpOverlapped == nullptr) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	std::unique_lock lock(pipe->connectMutex);

	if (pipe->clientConnected) {
		setLastError(ERROR_PIPE_CONNECTED);
		return FALSE;
	}

	if (pipe->companionFd < 0) {
		setLastError(ERROR_PIPE_BUSY);
		return FALSE;
	}

	if (pipe->connectPending) {
		setLastError(ERROR_PIPE_LISTENING);
		return FALSE;
	}

	if ((pipe->pipeMode & PIPE_NOWAIT) != 0) {
		if (pipe->requiresConnect) {
			pipe->requiresConnect = false;
			return TRUE;
		}
		setLastError(ERROR_PIPE_LISTENING);
		return FALSE;
	}

	pipe->requiresConnect = false;
	if (isOverlappedHandle) {
		pipe->connectPending = true;
		pipe->pendingOverlapped = lpOverlapped;
		pipe->pendingThread = pthread_self();
		lpOverlapped->Internal = STATUS_PENDING;
		lpOverlapped->InternalHigh = 0;
		kernel32::detail::resetOverlappedEvent(lpOverlapped);
		lock.unlock();
		setLastError(ERROR_IO_PENDING);
		return FALSE;
	}

	pipe->connectPending = true;
	pipe->connectCv.wait(lock, [&]() { return pipe->clientConnected || pipe->companionFd < 0; });
	pipe->connectPending = false;
	if (!pipe->clientConnected) {
		setLastError(ERROR_NO_DATA);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DisconnectNamedPipe(HANDLE hNamedPipe) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DisconnectNamedPipe(%p)\n", hNamedPipe);
	auto pipe = wibo::handles().getAs<NamedPipeInstance>(hNamedPipe);
	if (!pipe) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	std::lock_guard connectLock(pipe->connectMutex);
	if (!pipe->clientConnected) {
		pipe->requiresConnect = true;
		setLastError(ERROR_PIPE_LISTENING);
		return FALSE;
	}
	// A synchronous stream operation may be blocked on the existing descriptor.
	// Replacing it while that operation runs would race descriptor reuse.
	std::unique_lock streamLock(pipe->streamIoMutex, std::try_to_lock);
	if (!streamLock.owns_lock()) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}

	int endpoints[2] = {-1, -1};
	const DWORD access = pipe->accessMode & PIPE_ACCESS_DUPLEX;
	if ((access == PIPE_ACCESS_DUPLEX ? socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) : ::pipe(endpoints)) != 0) {
		setLastError(wibo::winErrorFromErrno(errno));
		return FALSE;
	}
	int serverFd = access == PIPE_ACCESS_OUTBOUND ? endpoints[1] : endpoints[0];
	int companionFd = access == PIPE_ACCESS_OUTBOUND ? endpoints[0] : endpoints[1];
	std::lock_guard fileLock(pipe->m);
	const int oldFd = pipe->fd;
	const int descriptorFlags = fcntl(oldFd, F_GETFD);
	const bool inherit = descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC) == 0;
	configureInheritability(serverFd, inherit);
	configureInheritability(companionFd, inherit);
#ifdef __linux__
	if (access != PIPE_ACCESS_DUPLEX) {
		const int capacity = fcntl(oldFd, F_GETPIPE_SZ);
		if (capacity > 0)
			fcntl(serverFd, F_SETPIPE_SZ, capacity);
	}
#endif
	pipe->fd = serverFd;
	pipe->companionFd = companionFd;
	pipe->clientConnected = false;
	pipe->requiresConnect = true;
	close(oldFd);
	return TRUE;
}

NamedPipeCancelResult cancelNamedPipeConnect(HANDLE handle, LPOVERLAPPED overlapped, bool callerThreadOnly) {
	auto pipe = wibo::handles().getAs<NamedPipeInstance>(handle);
	if (!pipe || !pipe->valid())
		return NamedPipeCancelResult::NotPipe;
	std::lock_guard lock(pipe->connectMutex);
	if (!pipe->connectPending || !pipe->pendingOverlapped || (overlapped && pipe->pendingOverlapped != overlapped) ||
		(callerThreadOnly && !pthread_equal(pipe->pendingThread, pthread_self())))
		return NamedPipeCancelResult::NotFound;
	kernel32::detail::signalOverlappedEvent(pipe.get(), pipe->pendingOverlapped, STATUS_CANCELLED, 0);
	pipe->pendingOverlapped = nullptr;
	pipe->connectPending = false;
	pipe->connectCv.notify_all();
	return NamedPipeCancelResult::Cancelled;
}

} // namespace kernel32
