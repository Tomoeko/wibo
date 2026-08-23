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
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/event.h>
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
				auto canonical = std::filesystem::weakly_canonical(buffer.c_str(), ec);
				if (!ec) {
					path = canonical.string();
				} else {
					path.assign(buffer.c_str());
				}
			}
		}
		if (path.empty()) {
			path = "wibo";
		}
	});
	return path;
}

class DarwinProcessManager final : public wibo::detail::ProcessManagerImpl {
  public:
	bool init() override;
	void shutdown() override;
	bool addProcess(Pin<ProcessObject> po) override;
	[[nodiscard]] bool running() const override { return mRunning.load(std::memory_order_acquire); }

  private:
	void runLoop();
	void wake() const;
	void handleExit(pid_t pid);
	bool registerProcess(pid_t pid, const Pin<ProcessObject> &process);
	Pin<ProcessObject> takeProcess(pid_t pid);

	mutable std::mutex m;
	std::atomic<bool> mRunning{false};
	std::thread mThread;
	int mKqueueFd = -1;
	static constexpr uintptr_t kWakeIdent = 1;
	std::unordered_map<pid_t, Pin<ProcessObject>> mProcesses;
};

void completeProcess(Pin<ProcessObject> process, int status) {
	{
		std::lock_guard lk(process->m);
		process->signaled = true;
		process->pidfd = -1;
		if (!process->forcedExitCode) {
			process->exitCode = decodeExitStatus(status);
		}
	}
	process->cv.notify_all();
	process->notifyWaiters(false);
}

} // namespace

namespace wibo::detail {

std::unique_ptr<ProcessManagerImpl> createProcessManagerImpl() { return std::make_unique<DarwinProcessManager>(); }

int spawnProcess(char *const argv[], char *const envp[], SpawnProcessInfo &info) {
	auto &path = executablePath();
	posix_spawnattr_t attr;
	int rc = posix_spawnattr_init(&attr);
	if (rc != 0) {
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
		rc = posix_spawn(&pid, path.c_str(), nullptr, &attr, argv, envp);
	}
	posix_spawnattr_destroy(&attr);
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
				handleExit(static_cast<pid_t>(event.ident));
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

void DarwinProcessManager::handleExit(pid_t pid) {
	auto process = takeProcess(pid);
	if (!process) {
		DEBUG_LOG("ProcessManager: exit event for unknown pid %d\n", pid);
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
