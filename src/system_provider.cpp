#include "system_provider.h"

#include "common.h"
#include "strutil.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
	const int spawnError = posix_spawn(&child, path, &actions, &attributes, argv.data(), environ);
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
