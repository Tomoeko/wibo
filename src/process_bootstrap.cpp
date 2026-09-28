#include "process_bootstrap.h"

#include "errors.h"
#include "kernel32/processenv.h"
#include "kernel32/wincon.h"
#include "processes.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint32_t kBootstrapMagic = 0x57425031;
constexpr uint32_t kBootstrapVersion = 4;
constexpr size_t kMaximumBootstrap = 16 * 1024 * 1024;
constexpr uint32_t kReadyMessage = 1;
constexpr uint32_t kThreadExitMessage = 2;

struct ControlMessage {
	uint32_t magic;
	uint32_t kind;
	uint32_t value;
};

int gChildControl = -1;
DWORD gPrimaryThreadId = 0;

void closeDescriptor(int &fd) {
	if (fd >= 0)
		close(std::exchange(fd, -1));
}

int transferBytes(int fd, void *buffer, size_t length, bool writing) {
	auto *bytes = static_cast<uint8_t *>(buffer);
	while (length) {
		const ssize_t count = writing ? write(fd, bytes, length) : read(fd, bytes, length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		bytes += count;
		length -= static_cast<size_t>(count);
	}
	return 0;
}

int sendMessage(int fd, const ControlMessage &message) {
#ifdef MSG_NOSIGNAL
	constexpr int flags = MSG_NOSIGNAL;
#else
	constexpr int flags = 0;
#endif
	ssize_t count;
	do {
		count = send(fd, &message, sizeof(message), flags);
	} while (count < 0 && errno == EINTR);
	return count == sizeof(message) ? 0 : count < 0 ? errno : EIO;
}

struct Writer {
	std::vector<uint8_t> bytes;
	void number(uint32_t value) {
		for (unsigned shift = 0; shift < 32; shift += 8)
			bytes.push_back(static_cast<uint8_t>(value >> shift));
	}
	void string(const std::string &value) {
		number(static_cast<uint32_t>(value.size()));
		bytes.insert(bytes.end(), value.begin(), value.end());
	}
};

struct Reader {
	std::span<const uint8_t> bytes;
	bool number(uint32_t &value) {
		if (bytes.size() < 4)
			return false;
		value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= static_cast<uint32_t>(bytes[index]) << (index * 8);
		bytes = bytes.subspan(4);
		return true;
	}
	bool string(std::string &value) {
		uint32_t length;
		if (!number(length) || length > bytes.size() || length > 32768)
			return false;
		value.assign(reinterpret_cast<const char *>(bytes.data()), length);
		bytes = bytes.subspan(length);
		return value.find('\0') == std::string::npos;
	}
};

bool configureDescriptor(int fd) { return fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0; }

int reserveDescriptor(int &fd) {
	if (fd < 0)
		return errno;
	if (fd < 3) {
		const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		if (replacement < 0)
			return errno;
		close(fd);
		fd = replacement;
	}
	return configureDescriptor(fd) ? 0 : errno;
}

} // namespace

namespace wibo::detail {

ProcessBootstrap::~ProcessBootstrap() {
	closeDescriptor(mManifest);
	closeDescriptor(mControl);
	closeDescriptor(mChildEndpoint);
	closeDescriptor(mConsole);
}

DWORD ProcessBootstrap::prepare(const SpawnOptions &options) {
	if (mManifest >= 0 || options.handles.size() > MAX_HANDLES || options.environment.size() > 1024 * 1024)
		return ERROR_INVALID_PARAMETER;

	char temporary[] = "/tmp/wibo-process-XXXXXX";
	mManifest = mkostemp(temporary, O_CLOEXEC);
	if (mManifest < 0)
		return winErrorFromErrno(errno);
	if (unlink(temporary) != 0 || reserveDescriptor(mManifest))
		return winErrorFromErrno(errno);
	int endpoints[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) != 0)
		return winErrorFromErrno(errno);
	mControl = endpoints[0];
	mChildEndpoint = endpoints[1];
	if (reserveDescriptor(mControl) || reserveDescriptor(mChildEndpoint))
		return winErrorFromErrno(errno);
	if (!options.detachedConsole) {
		const DWORD error = kernel32::snapshotConsoleSessionDescriptor(mConsole);
		if (error)
			return error;
	}
#ifdef SO_NOSIGPIPE
	int enabled = 1;
	if (setsockopt(mControl, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0 ||
		setsockopt(mChildEndpoint, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
		return winErrorFromErrno(errno);
#endif

	std::vector<kernel32::FileObject *> objects;
	for (const auto &entry : options.handles) {
		if (!entry.object || entry.object->type != ObjectType::File)
			return ERROR_NOT_SUPPORTED;
		auto file = entry.object.clone().downcast<kernel32::FileObject>();
		if (!file)
			return ERROR_NOT_SUPPORTED;
		if (std::find(objects.begin(), objects.end(), file.get()) == objects.end())
			objects.push_back(file.get());
	}
	int maximumSource = std::max({mManifest, mControl, mChildEndpoint, mConsole, STDERR_FILENO});
	for (auto *file : objects) {
		std::lock_guard lock(file->m);
		const DWORD error = files::prepareInheritanceLocked(*file);
		if (error)
			return error;
		maximumSource = std::max({maximumSource, file->fd, file->cursor.controlDescriptorLocked()});
	}
	const long descriptorLimit = sysconf(_SC_OPEN_MAX);
	if (descriptorLimit <= 0 || maximumSource + 4 + objects.size() * 2 >= static_cast<size_t>(descriptorLimit))
		return ERROR_NOT_SUPPORTED;
	int nextDescriptor = maximumSource + 1;
	const int childConsole = mConsole < 0 ? -1 : nextDescriptor++;
	if (childConsole >= 0)
		mDescriptors.push_back({mConsole, childConsole});

	Writer writer;
	writer.number(kBootstrapMagic);
	writer.number(kBootstrapVersion);
	writer.number(kernel32::isConsoleControlCIgnored());
	writer.number(options.detachedConsole);
	writer.number(childConsole < 0 ? UINT32_MAX : static_cast<uint32_t>(childConsole));
	writer.number(options.standardHandles.has_value());
	if (options.standardHandles) {
		writer.number(static_cast<uint32_t>(options.standardHandles->input));
		writer.number(static_cast<uint32_t>(options.standardHandles->output));
		writer.number(static_cast<uint32_t>(options.standardHandles->error));
		writer.number(options.standardHandles->explicitStartup);
	}
	writer.number(static_cast<uint32_t>(options.environment.size()));
	for (uint16_t unit : options.environment)
		writer.number(unit);
	writer.number(static_cast<uint32_t>(objects.size()));
	std::vector<int> dataDescriptors;
	for (auto *file : objects) {
		std::lock_guard lock(file->m);
		const DWORD error = files::prepareInheritanceLocked(*file);
		if (error)
			return error;
		const int data = nextDescriptor++;
		dataDescriptors.push_back(data);
		mDescriptors.push_back({file->fd, data});
		const int sourceControl = file->cursor.controlDescriptorLocked();
		const int control = sourceControl < 0 ? -1 : nextDescriptor++;
		if (control >= 0)
			mDescriptors.push_back({sourceControl, control});
		writer.number(data);
		writer.number(control);
		writer.number(file->shareAccess);
		writer.number(file->openFlags);
		writer.number(file->appendOnly);
		writer.string(file->canonicalPath.string());
	}
	writer.number(static_cast<uint32_t>(options.handles.size()));
	for (const auto &entry : options.handles) {
		const auto object = std::find(objects.begin(), objects.end(), entry.object.get());
		writer.number(static_cast<uint32_t>(entry.handle));
		writer.number(static_cast<uint32_t>(object - objects.begin()));
		writer.number(entry.grantedAccess);
		writer.number(entry.flags);
	}
	// Copy all sources before replacing native standard descriptors.
	if (options.standardHandles) {
		const std::array<HANDLE, 3> standard = {options.standardHandles->input, options.standardHandles->output,
												options.standardHandles->error};
		for (unsigned index = 0; index < standard.size(); ++index) {
			if (standard[index] == NO_HANDLE || standard[index] == static_cast<HANDLE>(-1)) {
				mDescriptors.push_back({-1, static_cast<int>(index)});
				continue;
			}
			const auto entry = std::find_if(options.handles.begin(), options.handles.end(),
											[&](const auto &item) { return item.handle == standard[index]; });
			if (entry == options.handles.end()) {
				if (options.standardHandles->explicitStartup)
					return ERROR_INVALID_HANDLE;
				mDescriptors.push_back({-1, static_cast<int>(index)});
				continue;
			}
			const auto object = std::find(objects.begin(), objects.end(), entry->object.get());
			mDescriptors.push_back({dataDescriptors[object - objects.begin()], static_cast<int>(index)});
		}
	}
	mChildManifest = nextDescriptor++;
	mChildControl = nextDescriptor++;
	mDescriptors.push_back({mManifest, mChildManifest});
	mDescriptors.push_back({mChildEndpoint, mChildControl});
	if (writer.bytes.size() > kMaximumBootstrap)
		return ERROR_INVALID_PARAMETER;
	int error = transferBytes(mManifest, writer.bytes.data(), writer.bytes.size(), true);
	if (error == 0 && lseek(mManifest, 0, SEEK_SET) < 0)
		error = errno;
	return error ? winErrorFromErrno(error) : 0;
}

int ProcessBootstrap::receiveReady(DWORD &threadId) {
	closeDescriptor(mChildEndpoint);
	closeDescriptor(mManifest);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	ControlMessage message{};
	size_t received = 0;
	while (received < sizeof(message)) {
		const auto remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0)
			return ETIMEDOUT;
		pollfd descriptor{mControl, POLLIN, 0};
		const int waited = poll(&descriptor, 1, static_cast<int>(remaining));
		if (waited < 0 && errno == EINTR)
			continue;
		if (waited <= 0)
			return waited == 0 ? ETIMEDOUT : errno;
		const ssize_t count =
			recv(mControl, reinterpret_cast<uint8_t *>(&message) + received, sizeof(message) - received, MSG_DONTWAIT);
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		received += static_cast<size_t>(count);
	}
	if (message.magic != kBootstrapMagic || message.kind != kReadyMessage || message.value == 0)
		return EIO;
	threadId = message.value;
	return 0;
}

int ProcessBootstrap::releaseControl() { return std::exchange(mControl, -1); }

} // namespace wibo::detail

namespace wibo {

DWORD initializeChildProcess(int manifestFd, int controlFd, std::optional<files::StandardHandles> &standardHandles,
						 ConsoleBootstrap &console) {
	struct stat information{};
	if (fstat(manifestFd, &information) != 0 || !S_ISREG(information.st_mode) || information.st_size < 12 ||
		information.st_size > static_cast<off_t>(kMaximumBootstrap) || !configureDescriptor(controlFd))
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> bytes(static_cast<size_t>(information.st_size));
	if (lseek(manifestFd, 0, SEEK_SET) < 0 || transferBytes(manifestFd, bytes.data(), bytes.size(), false))
		return ERROR_INVALID_DATA;
	close(manifestFd);
	Reader reader{bytes};
	uint32_t magic, version, ignoreControlC, detachedConsole, sessionDescriptor, standard;
	if (!reader.number(magic) || magic != kBootstrapMagic || !reader.number(version) || version != kBootstrapVersion ||
		!reader.number(ignoreControlC) || ignoreControlC > 1 || !reader.number(detachedConsole) || detachedConsole > 1 ||
		!reader.number(sessionDescriptor) || (detachedConsole && sessionDescriptor != UINT32_MAX) ||
		!reader.number(standard) || standard > 1)
		return ERROR_INVALID_DATA;
	if (standard) {
		uint32_t input, output, error, explicitStartup;
		if (!reader.number(input) || !reader.number(output) || !reader.number(error) ||
			!reader.number(explicitStartup) || explicitStartup > 1)
			return ERROR_INVALID_DATA;
		const auto decode = [](uint32_t value) {
			return value == UINT32_MAX ? static_cast<HANDLE>(-1) : static_cast<HANDLE>(value);
		};
		standardHandles = files::StandardHandles{decode(input), decode(output), decode(error), explicitStartup != 0};
	}
	uint32_t count;
	if (!reader.number(count) || count > 1024 * 1024)
		return ERROR_INVALID_DATA;
	std::vector<uint16_t> environment;
	environment.reserve(count);
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t unit;
		if (!reader.number(unit) || unit > UINT16_MAX)
			return ERROR_INVALID_DATA;
		environment.push_back(static_cast<uint16_t>(unit));
	}
	if (!reader.number(count) || count > MAX_HANDLES)
		return ERROR_INVALID_DATA;
	std::vector<Pin<kernel32::FileObject>> objects;
	std::vector<int> consumedDescriptors{controlFd};
	const auto consumeDescriptor = [&](uint32_t raw) {
		if (raw < 3 || raw > INT_MAX ||
			std::find(consumedDescriptors.begin(), consumedDescriptors.end(), raw) != consumedDescriptors.end() ||
			!configureDescriptor(static_cast<int>(raw)))
			return false;
		consumedDescriptors.push_back(static_cast<int>(raw));
		return true;
	};
	if (sessionDescriptor != UINT32_MAX && !consumeDescriptor(sessionDescriptor))
		return ERROR_INVALID_DATA;
	console.detached = detachedConsole != 0;
	console.inheritedDescriptor = sessionDescriptor == UINT32_MAX ? -1 : static_cast<int>(sessionDescriptor);
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t data, control, share, flags, append;
		std::string path;
		if (!reader.number(data) || !reader.number(control) || !reader.number(share) || !reader.number(flags) ||
			!reader.number(append) || append > 1 || !reader.string(path) || !consumeDescriptor(data))
			return ERROR_INVALID_DATA;
		auto file = make_pin<kernel32::FileObject>(static_cast<int>(data));
		file->shareAccess = share;
		file->openFlags = flags;
		file->appendOnly = append;
		file->canonicalPath = std::move(path);
		if (control != UINT32_MAX &&
			(!consumeDescriptor(control) || file->cursor.adoptControlDescriptor(static_cast<int>(control))))
			return ERROR_INVALID_DATA;
		objects.push_back(std::move(file));
	}
	if (!reader.number(count) || count > MAX_HANDLES)
		return ERROR_INVALID_DATA;
	std::vector<HandleTransferEntry> importedHandles;
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t handle, object, access, flags;
		if (!reader.number(handle) || !reader.number(object) || object >= objects.size() || !reader.number(access) ||
			!reader.number(flags))
			return ERROR_INVALID_DATA;
		importedHandles.push_back({static_cast<HANDLE>(handle), objects[object].clone(), access, flags});
	}
	if (!reader.bytes.empty())
		return ERROR_INVALID_DATA;
	if (standardHandles) {
		const std::array<HANDLE, 3> standardIds = {standardHandles->input, standardHandles->output,
												   standardHandles->error};
		for (unsigned index = 0; index < standardIds.size(); ++index) {
			if (standardIds[index] == NO_HANDLE || standardIds[index] == static_cast<HANDLE>(-1))
				continue;
			auto handle = std::find_if(importedHandles.begin(), importedHandles.end(),
									   [&](const auto &item) { return item.handle == standardIds[index]; });
			if (handle == importedHandles.end()) {
				if (standardHandles->explicitStartup)
					return ERROR_INVALID_DATA;
				continue;
			}
			if (!configureDescriptor(static_cast<int>(index)))
				return ERROR_INVALID_DATA;
			auto file = handle->object.clone().downcast<kernel32::FileObject>();
			if (!file)
				return ERROR_INVALID_DATA;
			file->ownedDescriptorAliases.push_back(static_cast<int>(index));
		}
	}
	DWORD error = kernel32::installChildEnvironment(environment);
	if (error == 0)
		error = wibo::handles().importExact(importedHandles);
	if (error)
		return error;
	// Handler registrations belong to the new process; only the ignore attribute is inherited.
	kernel32::initializeConsoleControlCIgnore(ignoreControlC != 0);
	gChildControl = controlFd;
	gPrimaryThreadId = getThreadId();
	const ControlMessage ready{kBootstrapMagic, kReadyMessage, gPrimaryThreadId};
	if (sendMessage(controlFd, ready))
		return ERROR_BROKEN_PIPE;
	char command;
	if (transferBytes(controlFd, &command, 1, false) || command != 'R')
		return ERROR_BROKEN_PIPE;
	return 0;
}

void reportPrimaryThreadExit(DWORD exitCode) {
	if (gChildControl >= 0 && getThreadId() == gPrimaryThreadId) {
		const ControlMessage message{kBootstrapMagic, kThreadExitMessage, exitCode};
		(void)sendMessage(gChildControl, message);
	}
}

std::shared_future<void> monitorPrimaryThread(int controlFd, Pin<kernel32::ProcessThreadObject> thread) {
	std::promise<void> completion;
	auto future = completion.get_future().share();
	std::thread([controlFd, thread = std::move(thread), completion = std::move(completion)]() mutable {
		ControlMessage message{};
		if (transferBytes(controlFd, &message, sizeof(message), false) == 0 && message.magic == kBootstrapMagic &&
			message.kind == kThreadExitMessage)
			thread->complete(message.value, true);
		close(controlFd);
		completion.set_value();
	}).detach();
	return future;
}

} // namespace wibo
