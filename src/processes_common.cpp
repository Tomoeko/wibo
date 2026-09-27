#include "processes.h"

#include "common.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "kernel32/processenv.h"
#include "strutil.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
extern char **environ;
#endif

using kernel32::ProcessObject;

namespace wibo {

std::string detail::snapshotProcessName(const std::string &hostImage, const std::string &runtimeImage,
										const std::vector<std::string> &arguments) {
	std::string image = hostImage;
	if (hostImage == runtimeImage) {
		std::string commandLine;
		bool options = true;
		for (size_t i = 1; i < arguments.size(); ++i) {
			const auto &arg = arguments[i];
			if (options && arg == "--") {
				options = false;
				continue;
			}
			if (options && arg == "--process-bootstrap") {
				if (arguments.size() - i <= 2)
					break;
				i += 2;
				continue;
			}
			if (options && (arg == "--cmdline" || arg == "--chdir" || arg == "-C")) {
				if (++i == arguments.size())
					break;
				if (arg == "--cmdline")
					commandLine = arguments[i];
				continue;
			}
			if (options && arg.starts_with("--cmdline=")) {
				commandLine = arg.substr(10);
				continue;
			}
			if (options && !arg.empty() && arg[0] == '-')
				continue;
			image = arg;
			break;
		}
		if (image == hostImage && !commandLine.empty()) {
			const auto split = splitCommandLine(commandLine.c_str());
			if (!split.empty())
				image = split[0];
		}
	}
	const auto slash = image.find_last_of("/\\");
	return slash == std::string::npos ? image : image.substr(slash + 1);
}

ProcessManager::ProcessManager() : mImpl(detail::createProcessManagerImpl()) {}

ProcessManager::~ProcessManager() { shutdown(); }

bool ProcessManager::init() {
	std::call_once(mInitOnce, [this] { mInitialized = mImpl && mImpl->init(); });
	return mInitialized;
}

void ProcessManager::shutdown() {
	if (mInitialized && mImpl) {
		mImpl->shutdown();
	}
}

bool ProcessManager::addProcess(Pin<ProcessObject> po) {
	if (!mImpl) {
		return false;
	}
	return mImpl->addProcess(std::move(po));
}

bool ProcessManager::running() const { return mImpl && mImpl->running(); }
Pin<ProcessObject> ProcessManager::findProcess(pid_t pid) {
	return mImpl ? mImpl->findProcess(pid) : Pin<ProcessObject>{};
}

int ProcessManager::openProcess(pid_t pid, Pin<ProcessObject> &process) {
	return mImpl ? mImpl->openProcess(pid, process) : ENOTSUP;
}

ProcessManager &processes() {
	static ProcessManager mgr;
	if (!mgr.init()) {
		std::fprintf(stderr, "Failed to initialize ProcessManager\n");
		std::abort();
	}
	return mgr;
}

static bool hasDirectoryComponent(const std::string &command) {
	return command.find('/') != std::string::npos || command.find('\\') != std::string::npos ||
		   command.find(':') != std::string::npos;
}

static bool hasExtension(const std::string &command) {
	auto pos = command.find_last_of('.');
	auto slash = command.find_last_of("/\\");
	return pos != std::string::npos && (slash == std::string::npos || pos > slash + 1);
}

static std::vector<std::string> pathextValues() {
	std::string raw = ".COM;.EXE;.BAT;.CMD";
	if (const auto value = kernel32::environmentValue(u"PATHEXT"))
		if (!utf16ToUtf8(*value, raw))
			return {};
	std::vector<std::string> exts;
	size_t start = 0;
	while (start <= raw.size()) {
		size_t end = raw.find(';', start);
		if (end == std::string::npos) {
			end = raw.size();
		}
		std::string part = raw.substr(start, end - start);
		if (!part.empty()) {
			if (part[0] != '.') {
				part.insert(part.begin(), '.');
			}
			exts.push_back(part);
		}
		if (end == raw.size()) {
			break;
		}
		start = end + 1;
	}
	if (exts.empty()) {
		exts = {".COM", ".EXE", ".BAT", ".CMD"};
	}
	return exts;
}

static std::vector<std::filesystem::path> parseHostPath(const std::string &value, bool windowsList = false) {
	std::vector<std::filesystem::path> paths;
	const char *delims = windowsList || std::strchr(value.c_str(), ';') ? ";" : ":";
	size_t start = 0;
	while (start <= value.size()) {
		size_t end = value.find_first_of(delims, start);
		if (end == std::string::npos) {
			end = value.size();
		}
		std::string entry = value.substr(start, end - start);
		if (!entry.empty()) {
			bool looksWindows =
				entry.find('\\') != std::string::npos || (entry.size() >= 2 && entry[1] == ':' && entry[0] != '/');
			std::filesystem::path candidate;
			if (looksWindows) {
				auto converted = files::pathFromWindows(entry.c_str());
				if (!converted.empty()) {
					candidate = converted;
				}
			}
			if (candidate.empty()) {
				candidate = std::filesystem::path(entry);
			}
			paths.push_back(std::move(candidate));
		}
		if (end == value.size()) {
			break;
		}
		start = end + 1;
	}
	return paths;
}

static std::vector<std::filesystem::path> buildSearchDirectories() {
	std::vector<std::filesystem::path> dirs;
	if (wibo::guestExecutablePath.has_parent_path()) {
		dirs.push_back(wibo::guestExecutablePath.parent_path());
	}
	dirs.push_back(std::filesystem::current_path());
	const auto system = files::systemSearchDirectories();
	for (const auto &directory : {system.system, system.legacySystem, system.windows})
		if (!directory.empty())
			dirs.push_back(directory);
	const auto addFromEnv = [&](const char *envVar) {
		if (const char *envPath = std::getenv(envVar)) {
			auto parsed = parseHostPath(envPath);
			dirs.insert(dirs.end(), parsed.begin(), parsed.end());
		}
	};
	addFromEnv("WIBO_PATH");
	addFromEnv("WINEPATH");
	if (const auto value = kernel32::environmentValue(u"PATH")) {
		std::string path;
		if (utf16ToUtf8(*value, path)) {
			auto parsed = parseHostPath(path, true);
			dirs.insert(dirs.end(), parsed.begin(), parsed.end());
		}
	}
	return dirs;
}

std::optional<std::filesystem::path> resolveExecutable(const std::string &command, bool searchPath,
													   ExecutablePathNamespace pathNamespace) {
	if (command.empty()) {
		return std::nullopt;
	}

	std::vector<std::string> candidates;
	candidates.push_back(command);
	if (!hasExtension(command)) {
		for (const auto &ext : pathextValues()) {
			candidates.push_back(command + ext);
		}
	}

	if (pathNamespace == ExecutablePathNamespace::Host && std::filesystem::path(command).is_absolute() &&
		!command.starts_with("//?/")) {
		for (const auto &name : candidates) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(name, ec) && !ec)
				return files::canonicalPath(name);
		}
		return std::nullopt;
	}

	auto tryResolveDirect = [&](const std::string &name) -> std::optional<std::filesystem::path> {
		auto host = files::pathFromWindows(name.c_str());
		if (host.empty()) {
			std::string normalized = name;
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			host = std::filesystem::path(normalized);
		}
		std::filesystem::path parent =
			host.parent_path().empty() ? std::filesystem::current_path() : host.parent_path();
		std::string filename = host.filename().string();
		auto resolved = files::findCaseInsensitiveFile(parent, filename);
		if (resolved) {
			return files::canonicalPath(*resolved);
		}
		std::error_code ec;
		if (!filename.empty() && std::filesystem::exists(host, ec)) {
			return files::canonicalPath(host);
		}
		return std::nullopt;
	};

	if (hasDirectoryComponent(command)) {
		for (const auto &name : candidates) {
			auto resolved = tryResolveDirect(name);
			if (resolved) {
				return resolved;
			}
		}
		return std::nullopt;
	}

	if (searchPath) {
		auto dirs = buildSearchDirectories();
		for (const auto &dir : dirs) {
			for (const auto &name : candidates) {
				auto resolved = files::findCaseInsensitiveFile(dir, name);
				if (resolved) {
					return files::canonicalPath(*resolved);
				}
			}
		}
	}

	return std::nullopt;
}

SpawnDirectory::~SpawnDirectory() {
	if (mFd >= 0)
		close(mFd);
}

int SpawnDirectory::open(const char *directory) {
	if (mFd >= 0) {
		close(mFd);
		mFd = -1;
	}
	if (!directory || !directory[0])
		return ENOENT;
	auto path = files::pathFromWindows(directory);
#ifdef __APPLE__
	constexpr int flags = O_SEARCH | O_DIRECTORY | O_CLOEXEC;
#else
	constexpr int flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
#endif
	do {
		mFd = ::open(path.c_str(), flags);
	} while (mFd < 0 && errno == EINTR);
	return mFd < 0 ? errno : 0;
}

static bool reapFailedSpawn(pid_t pid) {
	if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
		return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		int waitFlags = WNOHANG;
#ifdef __linux__
		waitFlags |= __WALL;
#endif
		const pid_t result = waitpid(pid, nullptr, waitFlags);
		if (result == pid || (result < 0 && errno == ECHILD))
			return true;
		if (result < 0 && errno != EINTR)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

static int spawnInternal(const std::vector<std::string> &args, Pin<kernel32::ProcessObject> &pinOut,
						 int directoryFd = -1, const SpawnOptions *options = nullptr, DWORD *bootstrapError = nullptr) {
	if (bootstrapError)
		*bootstrapError = 0;
	std::vector<std::string> preparedArgs;
	detail::ProcessBootstrap bootstrap;
	if (options) {
		const DWORD error = bootstrap.prepare(*options);
		if (error) {
			if (bootstrapError)
				*bootstrapError = error;
			return ENOTSUP;
		}
		preparedArgs = {"--process-bootstrap", std::to_string(bootstrap.childManifestDescriptor()),
						std::to_string(bootstrap.childControlDescriptor())};
	}
	preparedArgs.insert(preparedArgs.end(), args.begin(), args.end());
	std::vector<char *> argv;
	argv.reserve(preparedArgs.size() + 2);
	argv.push_back(const_cast<char *>("wibo"));
	for (auto &arg : preparedArgs) {
		argv.push_back(const_cast<char *>(arg.c_str()));
	}
	argv.push_back(nullptr);

	if (wibo::debugEnabled) {
		std::string cmdline;
		for (size_t i = 1; i < argv.size() - 1; ++i) {
			if (i != 1) {
				cmdline += ' ';
			}
			cmdline += '\'';
			cmdline += argv[i];
			cmdline += '\'';
		}
		DEBUG_LOG("Spawning process: %s %s\n", argv[0], cmdline.c_str());
	}

	std::vector<std::string> ownedEnv;
	ownedEnv.reserve(256);
	for (char **e = environ; *e; ++e) {
		if (std::strncmp(*e, "WIBO_DEBUG_INDENT=", 18) != 0)
			ownedEnv.emplace_back(*e);
	}
	ownedEnv.emplace_back("WIBO_DEBUG_INDENT=" + std::to_string(wibo::debugIndent + 1));

	std::vector<char *> envp;
	envp.reserve(ownedEnv.size() + 1);
	for (auto &s : ownedEnv)
		envp.push_back(const_cast<char *>(s.c_str()));
	envp.push_back(nullptr);

	detail::SpawnProcessInfo info;
	int rc = detail::spawnProcess(argv.data(), envp.data(), directoryFd, info, bootstrap.descriptors());
	if (rc != 0) {
		return rc;
	}

	DEBUG_LOG("Spawned process with PID %d (pidfd=%d)\n", info.pid, info.pidfd);

	DWORD threadId = 0;
	int control = -1;
	int resume = -1;
	if (options) {
		rc = bootstrap.receiveReady(threadId);
		if (rc == 0) {
			control = bootstrap.releaseControl();
			resume = fcntl(control, F_DUPFD_CLOEXEC, 3);
			if (resume < 0)
				rc = errno;
		}
		if (rc == 0 && !options->suspended) {
			const char command = 'R';
			int flags = 0;
#ifdef MSG_NOSIGNAL
			flags |= MSG_NOSIGNAL;
#endif
			ssize_t count;
			do {
				count = send(control, &command, 1, flags);
			} while (count < 0 && errno == EINTR);
			if (count != 1)
				rc = count < 0 ? errno : EIO;
		}
		if (rc) {
			if (control >= 0)
				close(control);
			if (resume >= 0)
				close(resume);
			if (!reapFailedSpawn(info.pid))
				DEBUG_LOG("Failed to reap child after bootstrap failure\n");
			if (info.pidfd >= 0)
				close(info.pidfd);
			return rc;
		}
	}
	auto obj = make_pin<kernel32::ProcessObject>(info.pid, info.pidfd, true);
	if (options) {
		obj->primaryThread = make_pin<kernel32::ProcessThreadObject>(threadId, resume, options->suspended);
		obj->primaryMonitor = monitorPrimaryThread(control, obj->primaryThread.clone());
	}
	if (!processes().addProcess(obj.clone())) {
		const bool reaped = reapFailedSpawn(info.pid);
		if (obj->primaryThread)
			obj->primaryThread->complete(0, false);
		DEBUG_LOG("Process registration failed; child reaped=%u\n", reaped);
		return reaped ? EIO : ETIMEDOUT;
	}
	pinOut = std::move(obj);
	return 0;
}

int spawnWithCommandLine(const std::string &applicationName, const std::string &commandLine,
						 Pin<kernel32::ProcessObject> &pinOut, int directoryFd, const SpawnOptions *options,
						 DWORD *bootstrapError) {
	if (applicationName.empty() && commandLine.empty()) {
		return ENOENT;
	}

	std::vector<std::string> args;
	args.reserve(3);
	if (!commandLine.empty()) {
		args.emplace_back("--cmdline");
		args.push_back(commandLine);
	}
	if (!applicationName.empty()) {
		args.push_back(applicationName);
	}

	return spawnInternal(args, pinOut, directoryFd, options, bootstrapError);
}

int spawnWithArgv(const std::string &applicationName, const std::vector<std::string> &argv,
				  Pin<kernel32::ProcessObject> &pinOut) {
	if (applicationName.empty() && argv.empty()) {
		return ENOENT;
	}

	std::vector<std::string> args;
	args.reserve(argv.size() + 1);
	if (!applicationName.empty()) {
		args.push_back(applicationName);
	}
	args.emplace_back("--");
	for (const auto &arg : argv) {
		args.push_back(arg);
	}

	return spawnInternal(args, pinOut);
}

std::vector<std::string> splitCommandLine(const char *commandLine) {
	std::vector<std::string> result;
	if (!commandLine) {
		return result;
	}
	std::string input(commandLine);
	size_t i = 0;
	size_t len = input.size();
	while (i < len) {
		while (i < len && (input[i] == ' ' || input[i] == '\t')) {
			++i;
		}
		if (i >= len) {
			break;
		}
		std::string arg;
		bool inQuotes = false;
		int backslashes = 0;
		for (; i < len; ++i) {
			char c = input[i];
			if (c == '\\') {
				++backslashes;
				continue;
			}
			if (c == '"') {
				if ((backslashes % 2) == 0) {
					arg.append(backslashes / 2, '\\');
					inQuotes = !inQuotes;
				} else {
					arg.append(backslashes / 2, '\\');
					arg.push_back('"');
				}
				backslashes = 0;
				continue;
			}
			arg.append(backslashes, '\\');
			backslashes = 0;
			if (!inQuotes && (c == ' ' || c == '\t')) {
				break;
			}
			arg.push_back(c);
		}
		arg.append(backslashes, '\\');
		result.push_back(std::move(arg));
		while (i < len && (input[i] == ' ' || input[i] == '\t')) {
			++i;
		}
	}
	return result;
}

DWORD getThreadId() {
#if defined(HAVE_PTHREAD_GETTID_NP)
	pid_t threadId = pthread_gettid_np(pthread_self());
#elif defined(__linux__)
	static thread_local pid_t threadId = gettid();
#elif defined(__APPLE__)
	uint64_t threadId = 0;
	pthread_threadid_np(nullptr, &threadId);
#else
#error "Unknown platform"
#endif
	return static_cast<DWORD>(threadId);
}

} // namespace wibo
