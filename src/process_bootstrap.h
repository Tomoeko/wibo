#pragma once

#include "files.h"
#include "handles.h"

#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <vector>

namespace wibo {

struct SpawnOptions {
	bool suspended = false;
	bool detachedConsole = false;
	std::vector<uint16_t> environment;
	std::vector<HandleTransferEntry> handles;
	std::optional<files::StandardHandles> standardHandles;
};

struct ConsoleBootstrap {
	bool detached = false;
	int inheritedDescriptor = -1;
};

namespace detail {

struct DescriptorMapping {
	int source;
	int destination;
};

class ProcessBootstrap {
  public:
	ProcessBootstrap() = default;
	~ProcessBootstrap();
	ProcessBootstrap(const ProcessBootstrap &) = delete;
	ProcessBootstrap &operator=(const ProcessBootstrap &) = delete;
	DWORD prepare(const SpawnOptions &options);
	[[nodiscard]] std::span<const DescriptorMapping> descriptors() const { return mDescriptors; }
	[[nodiscard]] int childManifestDescriptor() const { return mChildManifest; }
	[[nodiscard]] int childControlDescriptor() const { return mChildControl; }
	int receiveReady(DWORD &threadId);
	int releaseControl();

  private:
	int mManifest = -1;
	int mControl = -1;
	int mChildEndpoint = -1;
	int mChildManifest = -1;
	int mChildControl = -1;
	int mConsole = -1;
	std::vector<DescriptorMapping> mDescriptors;
};

} // namespace detail

// Imported state and the initial resume gate precede all guest module initialization.
DWORD initializeChildProcess(int manifestFd, int controlFd, std::optional<files::StandardHandles> &standardHandles,
						 ConsoleBootstrap &console);
std::shared_future<void> monitorPrimaryThread(int controlFd, Pin<kernel32::ProcessThreadObject> thread);
void reportPrimaryThreadExit(DWORD exitCode);

} // namespace wibo
