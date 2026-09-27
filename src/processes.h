#pragma once

#include "kernel32/internal.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using kernel32::ProcessObject;

namespace wibo {

struct ProcessSnapshotEntry {
	DWORD pid;
	DWORD parentPid;
	DWORD threadCount;
	LONG priority;
	std::string name;
};

int snapshotProcesses(std::vector<ProcessSnapshotEntry> &entries);

namespace detail {

class ProcessManagerImpl {
  public:
	virtual ~ProcessManagerImpl() = default;
	virtual bool init() = 0;
	virtual void shutdown() = 0;
	virtual bool addProcess(Pin<ProcessObject> po) = 0;
	virtual Pin<ProcessObject> findProcess(pid_t pid) = 0;
	virtual int openProcess(pid_t pid, Pin<ProcessObject> &process) = 0;
	[[nodiscard]] virtual bool running() const = 0;
};

struct SpawnProcessInfo {
	pid_t pid = -1;
	int pidfd = -1;
};

std::string snapshotProcessName(const std::string &hostImage, const std::string &runtimeImage,
								const std::vector<std::string> &arguments);
std::unique_ptr<ProcessManagerImpl> createProcessManagerImpl();
int spawnProcess(char *const argv[], char *const envp[], int directoryFd, SpawnProcessInfo &info);

} // namespace detail

class ProcessManager {
  public:
	ProcessManager();
	~ProcessManager();
	bool init();
	void shutdown();
	bool addProcess(Pin<ProcessObject> po);
	Pin<ProcessObject> findProcess(pid_t pid);
	int openProcess(pid_t pid, Pin<ProcessObject> &process);
	[[nodiscard]] bool running() const;

  private:
	std::unique_ptr<detail::ProcessManagerImpl> mImpl;
	std::once_flag mInitOnce;
	bool mInitialized = false;
};

ProcessManager &processes();

class SpawnDirectory {
  public:
	SpawnDirectory() = default;
	~SpawnDirectory();
	SpawnDirectory(const SpawnDirectory &) = delete;
	SpawnDirectory &operator=(const SpawnDirectory &) = delete;
	int open(const char *directory);
	[[nodiscard]] int nativeFd() const { return mFd; }

  private:
	int mFd = -1;
};

std::optional<std::filesystem::path> resolveExecutable(const std::string &command, bool searchPath);
int spawnWithCommandLine(const std::string &applicationName, const std::string &commandLine,
						 Pin<kernel32::ProcessObject> &pinOut, int directoryFd = -1);
int spawnWithArgv(const std::string &applicationName, const std::vector<std::string> &argv,
				  Pin<kernel32::ProcessObject> &pinOut);
std::vector<std::string> splitCommandLine(const char *commandLine);

DWORD getThreadId();

} // namespace wibo
