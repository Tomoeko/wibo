#include "system_provider.h"

#include "common.h"
#include "strutil.h"

#ifdef __APPLE__
#include "processes.h"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxRequest = 64 * 1024;
constexpr size_t kMaxArguments = 32;

void appendNumber(std::vector<uint8_t> &bytes, uint32_t value) {
	for (unsigned shift = 0; shift < 32; shift += 8)
		bytes.push_back(static_cast<uint8_t>(value >> shift));
}

bool makePipe(int (&descriptors)[2]) {
	if (pipe(descriptors) != 0)
		return false;
	for (int &descriptor : descriptors) {
		if (descriptor <= STDERR_FILENO) {
			const int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
			if (replacement < 0) {
				close(descriptors[0]);
				close(descriptors[1]);
				return false;
			}
			close(descriptor);
			descriptor = replacement;
		} else if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
			close(descriptors[0]);
			close(descriptors[1]);
			return false;
		}
	}
	return true;
}

class PersistentProvider {
	std::timed_mutex mutex;
	pid_t child = 0;
	int input = -1, output = -1;
	std::string path;

	void reset() {
		if (input >= 0)
			close(input);
		if (output >= 0)
			close(output);
		input = output = -1;
		if (child) {
			kill(-child, SIGKILL);
			while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
			}
			child = 0;
		}
		path.clear();
	}

	bool start(const char *executable) {
		int incoming[2], outgoing[2];
		if (!makePipe(incoming))
			return false;
		if (!makePipe(outgoing)) {
			close(incoming[0]);
			close(incoming[1]);
			return false;
		}
		const std::array<int, 4> descriptors{incoming[0], incoming[1], outgoing[0], outgoing[1]};
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, incoming[0], STDIN_FILENO);
		posix_spawn_file_actions_adddup2(&actions, outgoing[1], STDOUT_FILENO);
		for (int descriptor : descriptors)
			posix_spawn_file_actions_addclose(&actions, descriptor);
		posix_spawnattr_t attributes;
		posix_spawnattr_init(&attributes);
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
		posix_spawnattr_setpgroup(&attributes, 0);
		char serve[] = "--serve";
		char *argv[] = {const_cast<char *>(executable), serve, nullptr};
		int error;
		{
#ifdef __APPLE__
			std::lock_guard lock(wibo::detail::nativeProcessOperationMutex());
#endif
			error = posix_spawn(&child, executable, &actions, &attributes, argv, environ);
		}
		posix_spawnattr_destroy(&attributes);
		posix_spawn_file_actions_destroy(&actions);
		close(incoming[0]);
		close(outgoing[1]);
		input = incoming[1];
		output = outgoing[0];
		if (error || fcntl(input, F_SETFL, O_NONBLOCK) < 0 || fcntl(output, F_SETFL, O_NONBLOCK) < 0) {
			reset();
			return false;
		}
		path = executable;
		return true;
	}

	static bool transfer(int fd, uint8_t *bytes, size_t size, bool sending, Clock::time_point deadline,
						 bool *timedOut) {
		while (size) {
			const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
			if (remaining <= 0) {
				if (timedOut)
					*timedOut = true;
				return false;
			}
			pollfd descriptor{fd, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
			const int ready = poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining, INT32_MAX)));
			if (ready < 0 && errno == EINTR)
				continue;
			if (ready <= 0) {
				if (!ready && timedOut)
					*timedOut = true;
				return false;
			}
			const ssize_t count = sending ? write(fd, bytes, size) : read(fd, bytes, size);
			if (count < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (count <= 0)
				return false;
			bytes += count;
			size -= static_cast<size_t>(count);
		}
		return true;
	}

  public:
	~PersistentProvider() { reset(); }

	bool request(const char *executable, const std::vector<std::string> &arguments, std::vector<uint8_t> &response,
				 int timeoutMs, bool *timedOut) {
		if (arguments.empty() || arguments.size() > kMaxArguments)
			return false;
		std::vector<uint8_t> frame(4);
		appendNumber(frame, static_cast<uint32_t>(arguments.size()));
		for (const auto &argument : arguments) {
			if (argument.size() > kMaxRequest - 4 || frame.size() > kMaxRequest - argument.size() ||
				argument.find('\0') != std::string::npos)
				return false;
			appendNumber(frame, static_cast<uint32_t>(argument.size()));
			frame.insert(frame.end(), argument.begin(), argument.end());
		}
		const uint32_t length = static_cast<uint32_t>(frame.size() - 4);
		for (unsigned index = 0; index < 4; ++index)
			frame[index] = static_cast<uint8_t>(length >> (index * 8));
		const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
		std::unique_lock lock(mutex, std::defer_lock);
		if (!lock.try_lock_until(deadline)) {
			if (timedOut)
				*timedOut = true;
			return false;
		}
		if (child) {
			int status = 0;
			const pid_t result = waitpid(child, &status, WNOHANG);
			if (result == child || (result < 0 && errno == ECHILD)) {
				child = 0;
				reset();
			}
		}
		if (path != executable)
			reset();
		if (!child && !start(executable))
			return false;
		std::array<uint8_t, 4> header{};
		if (!transfer(input, frame.data(), frame.size(), true, deadline, timedOut) ||
			!transfer(output, header.data(), header.size(), false, deadline, timedOut)) {
			reset();
			return false;
		}
		const uint32_t size =
			header[0] | (uint32_t(header[1]) << 8) | (uint32_t(header[2]) << 16) | (uint32_t(header[3]) << 24);
		if (size < 12 || size > wibo::provider::kMaxResponse) {
			reset();
			return false;
		}
		response.resize(size);
		if (!transfer(output, response.data(), response.size(), false, deadline, timedOut)) {
			response.clear();
			reset();
			return false;
		}
		return true;
	}
};
} // namespace

namespace wibo::provider {

bool configured() {
	const char *path = std::getenv("WIBO_SYSTEM_PROVIDER");
	return path && *path;
}

std::string encodeBytes(std::string_view input) {
	std::string result;
	constexpr char digits[] = "0123456789abcdef";
	result.reserve(input.size() * 2);
	for (unsigned char byte : input) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 15]);
	}
	return result;
}

bool encodeUtf8(std::u16string_view input, std::string &output) { return utf16ToUtf8(input, output); }

bool request(const std::vector<std::string> &arguments, std::vector<uint8_t> &response, int timeoutMs, bool *timedOut) {
	response.clear();
	if (timedOut)
		*timedOut = false;
	const char *path = std::getenv("WIBO_SYSTEM_PROVIDER");
	if (!path || !*path || timeoutMs <= 0) {
		return false;
	}
	const char *persistent = std::getenv("WIBO_SYSTEM_PROVIDER_PERSISTENT");
	if (persistent && std::strcmp(persistent, "1") == 0) {
		static PersistentProvider client;
		return client.request(path, arguments, response, timeoutMs, timedOut);
	}
	int descriptors[2];
	if (pipe(descriptors) != 0) {
		return false;
	}
	for (int descriptor : descriptors) {
		fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	}
	std::vector<char *> argv{const_cast<char *>(path)};
	for (const auto &argument : arguments) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	posix_spawn_file_actions_addclose(&actions, descriptors[1]);
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&attributes, 0);
	pid_t child = 0;
	int spawnError;
	{
#ifdef __APPLE__
		std::lock_guard lock(wibo::detail::nativeProcessOperationMutex());
#endif
		spawnError = posix_spawn(&child, path, &actions, &attributes, argv.data(), environ);
	}
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	close(descriptors[1]);
	if (spawnError) {
		close(descriptors[0]);
		return false;
	}
	fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	bool eof = false;
	bool reaped = false;
	bool valid = true;
	int status = 0;
	while (!eof || !reaped) {
		const auto timeLeft =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (timeLeft <= 0) {
			if (timedOut)
				*timedOut = true;
			valid = false;
			break;
		}
		pollfd descriptor{descriptors[0], POLLIN, 0};
		const int polled = poll(&descriptor, eof ? 0 : 1, static_cast<int>(std::min<int64_t>(timeLeft, 20)));
		if (polled < 0 && errno != EINTR) {
			valid = false;
			break;
		}
		if (!eof && polled > 0) {
			uint8_t buffer[4096];
			const auto count = read(descriptors[0], buffer, sizeof(buffer));
			if (count > 0) {
				if (response.size() + static_cast<size_t>(count) > kMaxResponse) {
					valid = false;
					break;
				}
				response.insert(response.end(), buffer, buffer + count);
			} else if (count == 0) {
				eof = true;
			} else if (errno != EAGAIN && errno != EINTR) {
				valid = false;
				break;
			}
		}
		if (!reaped) {
			const pid_t waited = waitpid(child, &status, WNOHANG);
			reaped = waited == child;
			if (waited < 0 && errno != EINTR) {
				valid = false;
				break;
			}
		}
	}
	close(descriptors[0]);
	if (!valid) {
		kill(-child, SIGKILL);
	}
	if (!reaped) {
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
	}
	valid = valid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (!valid) {
		response.clear();
		DEBUG_LOG("System provider request failed\n");
	}
	return valid;
}

bool Reader::number(uint32_t &value) {
	if (remaining.size() < 4) {
		return false;
	}
	value =
		remaining[0] | (uint32_t(remaining[1]) << 8) | (uint32_t(remaining[2]) << 16) | (uint32_t(remaining[3]) << 24);
	remaining = remaining.subspan(4);
	return true;
}

bool Reader::bytes(std::vector<uint8_t> &value) {
	uint32_t size = 0;
	if (!number(size) || size > remaining.size()) {
		return false;
	}
	value.assign(remaining.begin(), remaining.begin() + size);
	remaining = remaining.subspan(size);
	return true;
}

bool Reader::text(std::u16string &value) {
	std::vector<uint8_t> data;
	if (!bytes(data) || data.size() % 2) {
		return false;
	}
	value.clear();
	for (size_t i = 0; i < data.size(); i += 2) {
		value.push_back(static_cast<char16_t>(data[i] | (uint16_t(data[i + 1]) << 8)));
	}
	return true;
}

bool Reader::header(int32_t &status) {
	uint32_t magic = 0, version = 0, result = 0;
	if (!number(magic) || !number(version) || !number(result) || magic != kMagic || version != kVersion) {
		return false;
	}
	status = static_cast<int32_t>(result);
	return true;
}

} // namespace wibo::provider
