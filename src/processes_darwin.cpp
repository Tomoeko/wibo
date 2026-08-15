#include "processes.h"

#include "common.h"
#include "handles.h"
#include "kernel32/internal.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <system_error>
#include <string>
#include <sys/time.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>

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
	std::atomic<bool> mRunning{false};
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

struct ReaperContext {
	pid_t pid;
	Pin<ProcessObject> process;
};

void *reapProcess(void *rawContext) {
	std::unique_ptr<ReaperContext> context(static_cast<ReaperContext *>(rawContext));
	int status = 0;
	for (;;) {
		pid_t result = waitpid(context->pid, &status, 0);
		if (result == context->pid) {
			completeProcess(std::move(context->process), status);
			return nullptr;
		}
		if (result < 0 && errno == EINTR) {
			continue;
		}
		if (result < 0) {
			DEBUG_LOG("ProcessManager: waitpid(%d) failed: %s\n", context->pid, strerror(errno));
		}
		return nullptr;
	}
}

} // namespace

namespace wibo::detail {

std::unique_ptr<ProcessManagerImpl> createProcessManagerImpl() {
	return std::make_unique<DarwinProcessManager>();
}

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
	mRunning.store(true, std::memory_order_release);
	DEBUG_LOG("ProcessManager (Darwin) initialized\n");
	return true;
}

void DarwinProcessManager::shutdown() {
	mRunning.store(false, std::memory_order_release);
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
	auto *context = new ReaperContext{pid, std::move(po)};
	pthread_t thread;
	int result = pthread_create(&thread, nullptr, reapProcess, context);
	if (result != 0) {
		DEBUG_LOG("ProcessManager: failed to start reaper for pid %d: %s\n", pid, strerror(result));
		delete context;
		return false;
	}
	pthread_detach(thread);
	DEBUG_LOG("ProcessManager: registered pid %d\n", pid);
	return true;
}
