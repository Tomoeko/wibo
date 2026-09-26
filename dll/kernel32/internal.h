#pragma once

#include "common.h"
#include "handles.h"
#include "types.h"

#include <condition_variable>
#include <pthread.h>

namespace kernel32 {

struct FsObject : ObjectBase {
	std::mutex m;
	int fd = -1;
	std::filesystem::path canonicalPath;
	uint32_t shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	bool deletePending = false;
	bool closeOnDestroy = true;

	~FsObject() override;
	[[nodiscard]] bool valid() const { return fd >= 0; }

  protected:
	explicit FsObject(ObjectType type, int fd) : ObjectBase(type), fd(fd) { flags |= Of_FsObject; }
};

struct FileRangeLock {
	uint64_t start;
	uint64_t end;
	bool exclusive;
};

struct FileObject : FsObject {
	std::vector<FileRangeLock> rangeLocks;
	off_t filePos = 0;
	bool overlapped = false;
	bool appendOnly = false;
	bool isPipe = false;
	bool pipeMessageMode = false;
	// Used to notify overlapped operations without an event handle
	std::condition_variable overlappedCv;

	explicit FileObject(int fd) : FileObject(ObjectType::File, fd) {}
	FileObject(ObjectType type, int fd) : FsObject(type, fd) {
		flags |= Of_File;
		if (fd >= 0) {
			off_t pos = lseek(fd, 0, SEEK_CUR);
			if (pos == -1 && errno == ESPIPE) {
				isPipe = true;
			} else if (pos >= 0) {
				filePos = pos;
			}
		}
	}

	~FileObject() override = default;
};

struct DirectoryObject final : FsObject {
	static constexpr ObjectType kType = ObjectType::Directory;

	uint64_t enumCookie = 0;

	explicit DirectoryObject(int dirfd) : FsObject(kType, dirfd) {}
};

struct ProcessObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Process;

	pid_t pid;
	int pidfd;
	DWORD exitCode = STILL_ACTIVE;
	bool forcedExitCode = false;
	bool waitable = true;

	explicit ProcessObject(pid_t pid, int pidfd, bool waitable = true)
		: WaitableObject(kType), pid(pid), pidfd(pidfd), waitable(waitable) {}

	~ProcessObject() override {
		if (pidfd != -1) {
			close(pidfd);
			pidfd = -1;
		}
	}
};

#ifdef __linux__
constexpr pthread_t pthread_null = 0;
#else
constexpr pthread_t pthread_null = nullptr;
#endif

struct ApcState final : WaitableObject {
	struct Entry {
		GUEST_PTR callback;
		ULONG_PTR argument;
	};
	std::deque<Entry> pending;
	bool terminated = false;
	ApcState() : WaitableObject(ObjectType::ApcQueue) {}
};

std::shared_ptr<ApcState> currentApcState();
void installApcState(std::shared_ptr<ApcState> state);
void closeApcState();
bool dispatchPendingApcs();
DWORD waitAlertable(HANDLE handle, WaitableObject *object, DWORD milliseconds);

bool createWorkerThread(DWORD (*function)(void *), void *parameter, DWORD &error);

struct ThreadObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Thread;

	pthread_t thread;
	DWORD threadId = 0;
	bool initialized = false;
	DWORD exitCode = STILL_ACTIVE;
	unsigned int suspendCount = 0;
	TEB *tib = nullptr;
	std::shared_ptr<ApcState> apc = std::make_shared<ApcState>();

	explicit ThreadObject(pthread_t thread = pthread_null) : WaitableObject(kType), thread(thread) {}

	~ThreadObject() override {
		// Threads are detached at creation; we can safely drop
		if (tib) {
			wibo::destroyTib(tib);
			tib = nullptr;
		}
	}
};

struct MutexObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Mutex;

	bool ownerValid = false;
	pthread_t owner{};
	unsigned int recursionCount = 0;
	bool abandoned = false; // Owner exited without releasing

	MutexObject() : WaitableObject(kType) { signaled = true; }
};

struct EventObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Event;

	bool manualReset = false;

	explicit EventObject(bool manual) : WaitableObject(kType), manualReset(manual) {}

	void set() {
		bool resetAll = false;
		{
			std::lock_guard lk(m);
			signaled = true;
			resetAll = manualReset;
		}
		if (resetAll) {
			cv.notify_all();
		} else {
			cv.notify_one();
		}
		notifyWaiters(false);
	}

	void reset() {
		std::lock_guard lk(m);
		signaled = false;
	}
};

struct SemaphoreObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Semaphore;

	LONG count = 0;
	LONG maxCount = 0;

	SemaphoreObject(LONG initial, LONG maximum) : WaitableObject(kType), count(initial), maxCount(maximum) {}
};

struct TimerObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Timer;
	const bool manualReset;
	explicit TimerObject(bool manual) : WaitableObject(kType), manualReset(manual) {}
	void onLastHandleClosed() noexcept override;
};

struct HeapObject : public ObjectBase {
	static constexpr ObjectType kType = ObjectType::Heap;

	bool active = true;
	DWORD createFlags = 0;
	SIZE_T initialSize = 0;
	SIZE_T maximumSize = 0;
	DWORD compatibility = 0;
	bool isProcessHeap = false;

	HeapObject() : ObjectBase(kType) {}
	~HeapObject() override;

	[[nodiscard]] inline bool canAccess() const { return active; }
};

inline constexpr HANDLE kPseudoCurrentProcessHandleValue = static_cast<HANDLE>(-1);
inline constexpr HANDLE kPseudoCurrentThreadHandleValue = static_cast<HANDLE>(-2);

inline bool isPseudoCurrentProcessHandle(HANDLE h) { return h == kPseudoCurrentProcessHandleValue; }

inline bool isPseudoCurrentThreadHandle(HANDLE h) { return h == kPseudoCurrentThreadHandleValue; }

void tryMarkExecutable(void *mem);
void setLastErrorFromErrno();
[[noreturn]] void exitInternal(DWORD exitCode);

DWORD getLastError();
void setLastError(DWORD error);

} // namespace kernel32

namespace detail {

template <> constexpr bool typeMatches<kernel32::FsObject>(const ObjectBase *o) noexcept {
	return o && (o->flags & Of_FsObject);
}

template <> constexpr bool typeMatches<kernel32::FileObject>(const ObjectBase *o) noexcept {
	return o && (o->flags & Of_File);
}

} // namespace detail
