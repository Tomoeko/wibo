#include "processes.h"

#include "common.h"
#include "handles.h"
#include "kernel32/internal.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <libproc.h>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#define ENUM_DYLD_BOOL
#include <mach-o/dyld.h>

using kernel32::ProcessObject;

namespace {

DWORD decodeExitStatus(int status) {
	if (WIFEXITED(status)) {
		return static_cast<DWORD>(WEXITSTATUS(status));
	}
	if (WIFSIGNALED(status)) {
		return 0xC0000000u | static_cast<DWORD>(WTERMSIG(status));
	}
	return 0;
}

std::string &executablePath() {
	static std::string path;
	static std::once_flag once;
	std::call_once(once, [] {
		uint32_t size = 0;
		if (_NSGetExecutablePath(nullptr, &size) != 0 && size > 0) {
			std::string buffer(size, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
				std::error_code ec;
				auto absolute = std::filesystem::absolute(buffer.c_str(), ec);
				if (ec)
					return;
				auto canonical = std::filesystem::weakly_canonical(absolute, ec);
				if (!ec) {
					path = canonical.string();
				} else {
					path = absolute.string();
				}
			}
		}
	});
	return path;
}

class DarwinProcessManager final : public wibo::detail::ProcessManagerImpl {
  public:
	bool init() override;
	void shutdown() override;
	bool addProcess(Pin<ProcessObject> po) override;
	Pin<ProcessObject> findProcess(pid_t pid) override;
	int openProcess(pid_t pid, Pin<ProcessObject> &process) override;
	[[nodiscard]] bool running() const override { return mRunning.load(std::memory_order_acquire); }

  private:
	void runLoop();
	void wake() const;
	void handleExit(const struct kevent &event);
	bool registerProcess(pid_t pid, const Pin<ProcessObject> &process);
	Pin<ProcessObject> takeProcess(pid_t pid);

	mutable std::mutex m;
	std::atomic<bool> mRunning{false};
	std::thread mThread;
	int mKqueueFd = -1;
	static constexpr uintptr_t kWakeIdent = 1;
	std::unordered_map<pid_t, Pin<ProcessObject>> mProcesses;
};

void completeProcess(Pin<ProcessObject> process, int status, bool statusKnown = true) {
	{
		std::lock_guard lk(process->m);
		process->signaled = true;
		process->pidfd = -1;
		if (!process->forcedExitCode) {
			process->exitCodeKnown = statusKnown;
			if (statusKnown)
				process->exitCode = decodeExitStatus(status);
		}
	}
	process->cv.notify_all();
	process->notifyWaiters(false);
}

} // namespace

namespace wibo::detail {

std::unique_ptr<ProcessManagerImpl> createProcessManagerImpl() { return std::make_unique<DarwinProcessManager>(); }

int spawnProcess(char *const argv[], char *const envp[], int directoryFd, SpawnProcessInfo &info) {
	auto &path = executablePath();
	if (path.empty())
		return ENOENT;
	posix_spawn_file_actions_t actions;
	int rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0)
		return rc;
	if (directoryFd >= 0)
		rc = posix_spawn_file_actions_addfchdir_np(&actions, directoryFd);
	if (rc != 0) {
		posix_spawn_file_actions_destroy(&actions);
		return rc;
	}
	posix_spawnattr_t attr;
	rc = posix_spawnattr_init(&attr);
	if (rc != 0) {
		posix_spawn_file_actions_destroy(&actions);
		return rc;
	}
	sigset_t mask;
	sigemptyset(&mask);
	rc = posix_spawnattr_setsigmask(&attr, &mask);
	if (rc == 0) {
		rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
	}
	pid_t pid = -1;
	if (rc == 0) {
		rc = posix_spawn(&pid, path.c_str(), &actions, &attr, argv, envp);
	}
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		return rc;
	}
	info.pid = pid;
	info.pidfd = -1;
	return 0;
}

} // namespace wibo::detail

bool DarwinProcessManager::init() {
	if (mRunning.load(std::memory_order_acquire)) {
		return true;
	}

	mKqueueFd = kqueue();
	if (mKqueueFd < 0) {
		perror("kqueue");
		return false;
	}

	struct kevent event;
	EV_SET(&event, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
	if (kevent(mKqueueFd, &event, 1, nullptr, 0, nullptr) < 0) {
		perror("kevent(EV_ADD user)");
		close(mKqueueFd);
		mKqueueFd = -1;
		return false;
	}

	mRunning.store(true, std::memory_order_release);
	mThread = std::thread(&DarwinProcessManager::runLoop, this);
	DEBUG_LOG("ProcessManager (Darwin) initialized\n");
	return true;
}

void DarwinProcessManager::shutdown() {
	if (!mRunning.exchange(false, std::memory_order_acq_rel)) {
		return;
	}
	wake();
	if (mThread.joinable()) {
		mThread.join();
	}
	{
		std::lock_guard lk(m);
		mProcesses.clear();
	}
	if (mKqueueFd >= 0) {
		close(mKqueueFd);
		mKqueueFd = -1;
	}
}

bool DarwinProcessManager::registerProcess(pid_t pid, const Pin<ProcessObject> &process) {
	std::lock_guard lk(m);
	return mProcesses.emplace(pid, process.clone()).second;
}
Pin<ProcessObject> DarwinProcessManager::findProcess(pid_t pid) {
	std::lock_guard lock(m);
	const auto found = mProcesses.find(pid);
	return found == mProcesses.end() ? Pin<ProcessObject>{} : found->second.clone();
}

int DarwinProcessManager::openProcess(pid_t pid, Pin<ProcessObject> &process) {
	std::lock_guard lock(m);
	const auto found = mProcesses.find(pid);
	if (found != mProcesses.end()) {
		process = found->second.clone();
		return 0;
	}
	if (!mRunning.load(std::memory_order_acquire))
		return ENOTSUP;
	if (kill(pid, 0) != 0)
		return errno;
	auto object = make_pin<ProcessObject>(pid, -1);
	object->childProcess = false;
	struct kevent event{};
	// NOTE_EXITSTATUS checks native signal permissions and returns the wait status
	// without reaping a process belonging to another parent.
	EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXITSTATUS, 0,
		   nullptr);
	if (kevent(mKqueueFd, &event, 1, nullptr, 0, nullptr) < 0)
		return errno;
	// The monitor takes this lock before consuming an exit notification.
	mProcesses.emplace(pid, object.clone());
	process = std::move(object);
	DEBUG_LOG("ProcessManager: monitoring external pid %d\n", pid);
	return 0;
}

Pin<ProcessObject> DarwinProcessManager::takeProcess(pid_t pid) {
	std::lock_guard lk(m);
	auto it = mProcesses.find(pid);
	if (it == mProcesses.end()) {
		return {};
	}
	auto process = std::move(it->second);
	mProcesses.erase(it);
	return process;
}

bool DarwinProcessManager::addProcess(Pin<ProcessObject> po) {
	if (!po || !mRunning.load(std::memory_order_acquire)) {
		return false;
	}
	pid_t pid;
	{
		std::lock_guard lk(po->m);
		pid = po->pid;
	}

	// Publish ownership before enabling notifications so a short-lived child
	// cannot be observed by the monitor before its waitable object is visible.
	if (!registerProcess(pid, po)) {
		DEBUG_LOG("ProcessManager: pid %d is already registered\n", pid);
		return false;
	}

	struct kevent event;
	EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
	if (kevent(mKqueueFd, &event, 1, nullptr, 0, nullptr) < 0) {
		int error = errno;
		auto process = takeProcess(pid);
		DEBUG_LOG("ProcessManager: kevent add for pid %d failed: %s\n", pid, strerror(error));
		if (error == ESRCH && process) {
			int status = 0;
			pid_t result;
			do {
				result = waitpid(pid, &status, 0);
			} while (result < 0 && errno == EINTR);
			if (result == pid) {
				completeProcess(std::move(process), status);
				return true;
			}
		}
		return false;
	}

	DEBUG_LOG("ProcessManager: registered pid %d\n", pid);
	return true;
}

void DarwinProcessManager::runLoop() {
	constexpr int kMaxEvents = 64;
	std::array<struct kevent, kMaxEvents> events{};
	while (mRunning.load(std::memory_order_acquire)) {
		int count = kevent(mKqueueFd, nullptr, 0, events.data(), kMaxEvents, nullptr);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("kevent");
			break;
		}
		for (int i = 0; i < count; ++i) {
			const auto &event = events[i];
			if (event.filter == EVFILT_USER) {
				continue;
			}
			if (event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT)) {
				handleExit(event);
			}
		}
	}
}

void DarwinProcessManager::wake() const {
	if (mKqueueFd < 0) {
		return;
	}
	struct kevent event;
	EV_SET(&event, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
	kevent(mKqueueFd, &event, 1, nullptr, 0, nullptr);
}

void DarwinProcessManager::handleExit(const struct kevent &event) {
	const auto pid = static_cast<pid_t>(event.ident);
	auto process = takeProcess(pid);
	if (!process) {
		DEBUG_LOG("ProcessManager: exit event for unknown pid %d\n", pid);
		return;
	}

	if (!process->childProcess) {
		completeProcess(std::move(process), static_cast<int>(event.data & NOTE_PDATAMASK),
						(event.fflags & NOTE_EXITSTATUS) != 0);
		return;
	}
	int status = 0;
	pid_t result;
	do {
		result = waitpid(pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	if (result != pid) {
		DEBUG_LOG("ProcessManager: waitpid(%d) failed: %s\n", pid, strerror(errno));
		return;
	}
	completeProcess(std::move(process), status);
}

namespace {
std::vector<std::string> processArguments(pid_t pid) {
	int argmax = 0;
	size_t size = sizeof(argmax);
	int limit[] = {CTL_KERN, KERN_ARGMAX};
	if (sysctl(limit, 2, &argmax, &size, nullptr, 0) != 0 || argmax < static_cast<int>(sizeof(int)) ||
		argmax > 8 * 1024 * 1024)
		return {};
	std::vector<char> bytes(argmax);
	size = bytes.size();
	int query[] = {CTL_KERN, KERN_PROCARGS2, pid};
	if (sysctl(query, 3, bytes.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
		return {};
	int argc;
	std::memcpy(&argc, bytes.data(), sizeof(argc));
	if (argc < 0 || argc > 65536)
		return {};
	const char *cursor = bytes.data() + sizeof(argc);
	const char *end = bytes.data() + size;
	const auto *imageEnd = static_cast<const char *>(std::memchr(cursor, 0, end - cursor));
	if (!imageEnd)
		return {};
	cursor = imageEnd + 1;
	while (cursor < end && !*cursor)
		++cursor;
	std::vector<std::string> arguments;
	for (int i = 0; i < argc && cursor < end; ++i) {
		const auto *argEnd = static_cast<const char *>(std::memchr(cursor, 0, end - cursor));
		if (!argEnd)
			return {};
		arguments.emplace_back(cursor, argEnd);
		cursor = argEnd + 1;
	}
	return arguments;
}
} // namespace

int wibo::snapshotProcesses(std::vector<ProcessSnapshotEntry> &entries) {
	const int count = proc_listallpids(nullptr, 0);
	if (count <= 0)
		return errno ? errno : EIO;
	std::vector<pid_t> pids(static_cast<size_t>(count) + 32);
	int actual;
	for (;;) {
		actual = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
		if (actual <= 0)
			return errno ? errno : EIO;
		if (static_cast<size_t>(actual) < pids.size())
			break;
		if (pids.size() > 1024 * 1024)
			return EOVERFLOW;
		pids.resize(pids.size() * 2);
	}
	entries.clear();
	for (int i = 0; i < actual; ++i) {
		if (pids[i] <= 0)
			continue;
		proc_taskallinfo info{};
		if (proc_pidinfo(pids[i], PROC_PIDTASKALLINFO, 0, &info, sizeof(info)) != sizeof(info))
			continue; // Processes may exit or deny access while the snapshot is collected.
		ProcessSnapshotEntry entry{};
		entry.pid = info.pbsd.pbi_pid;
		entry.parentPid = info.pbsd.pbi_ppid;
		entry.threadCount = static_cast<DWORD>(info.ptinfo.pti_threadnum);
		// Scheduling priorities retain the native kernel's numeric scale.
		entry.priority = info.ptinfo.pti_priority;
		std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
		if (proc_pidpath(pids[i], path.data(), path.size()) > 0) {
			std::string hostImage = path.data();
			entry.name = detail::snapshotProcessName(hostImage, executablePath(),
													 hostImage == executablePath() ? processArguments(pids[i])
																				   : std::vector<std::string>{});
		} else {
			entry.name.assign(info.pbsd.pbi_name, strnlen(info.pbsd.pbi_name, sizeof(info.pbsd.pbi_name)));
		}
		if (pids[i] == getpid())
			entry.name = wibo::guestExecutablePath.filename().string();
		entries.push_back(std::move(entry));
	}
	return 0;
}
