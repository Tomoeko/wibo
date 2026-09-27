#pragma once

#include "common.h"
#include "handles.h"
#include "types.h"

#include <condition_variable>
#include <future>
#include <pthread.h>

namespace files {
struct FileShareLease;
}

namespace kernel32 {
void initializeEnvironment();

struct CompletionBinding;
class DirectoryWatcher;

struct FsObject : ObjectBase {
	std::mutex m;
	std::mutex overlappedMutex;
	std::condition_variable overlappedCv;
	std::shared_ptr<const CompletionBinding> completion;
	bool overlapped = false;
	int fd = -1;
	std::filesystem::path canonicalPath;
	uint32_t shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	DWORD openFlags = 0;
	bool deletePending = false;
	bool closeOnDestroy = true;
	std::shared_ptr<files::FileShareLease> shareLease;

	~FsObject() override;
	[[nodiscard]] bool valid() const { return fd >= 0; }

  protected:
	explicit FsObject(ObjectType type, int fd) : ObjectBase(type), fd(fd) { flags |= Of_FsObject; }
};

// The caller must hold file.m and retain the file object throughout the query.
NTSTATUS queryStandardInformationLocked(FsObject &file, FILE_STANDARD_INFORMATION &information);

struct FileRangeLock {
	uint64_t start;
	uint64_t end;
	bool exclusive;
};

class FileCursor {
  public:
	FileCursor() = default;
	FileCursor(const FileCursor &) = delete;
	FileCursor &operator=(const FileCursor &) = delete;
	~FileCursor();

	// The file object's mutex must protect these operations and descriptor transfer.
	int prepareTransferLocked();
	// Ownership transfers only on success. There must be one descriptor per object.
	int adoptControlDescriptor(int fd);
	[[nodiscard]] int controlDescriptorLocked() const { return mControlFd; }
	int lockOperationLocked() const;
	int unlockOperationLocked() const;

  private:
	int mControlFd = -1;
};

struct FileObject : FsObject {
	// Blocking stream I/O must not hold the mutex used by descriptor inheritance.
	std::mutex streamIoMutex;
	std::vector<FileRangeLock> rangeLocks;
	FileCursor cursor;
	// Additional inherited standard descriptors belong to the same file object.
	std::vector<int> ownedDescriptorAliases;
	bool appendOnly = false;
	bool isPipe = false;
	bool pipeMessageMode = false;

	explicit FileObject(int fd) : FileObject(ObjectType::File, fd) {}
	FileObject(ObjectType type, int fd) : FsObject(type, fd) {
		flags |= Of_File;
		if (fd >= 0) {
			off_t pos = lseek(fd, 0, SEEK_CUR);
			if (pos == -1 && errno == ESPIPE) {
				isPipe = true;
			}
		}
	}

	~FileObject() override;
};

struct DirectoryObject final : FsObject {
	static constexpr ObjectType kType = ObjectType::Directory;

	std::vector<std::string> enumEntries;
	std::u16string enumPattern;
	size_t enumCookie = 0;
	bool enumStarted = false;
	bool watchClosed = false;
	std::condition_variable changesCv;
	std::shared_ptr<DirectoryWatcher> watcher;
	~DirectoryObject() override;
	void onLastHandleClosed() noexcept override;

	explicit DirectoryObject(int dirfd) : FsObject(kType, dirfd) {}
};

struct ProcessThreadObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::ProcessThread;

	DWORD threadId;
	DWORD exitCode = STILL_ACTIVE;
	bool exitCodeKnown = true;
	unsigned int suspendCount;

	ProcessThreadObject(DWORD threadId, int resumeFd, bool suspended);
	~ProcessThreadObject() override;
	void onLastHandleClosed() noexcept override;
	DWORD resumeInitial();
	void complete(DWORD code, bool known);

  private:
	int mResumeFd;
	int mResumeError = 0;
};

HANDLE allocateProcessThreadHandle(Pin<ProcessThreadObject> thread, DWORD access, DWORD flags);
HANDLE openProcessThreadHandle(DWORD threadId, DWORD access, DWORD flags);

struct ProcessObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Process;

	pid_t pid;
	int pidfd;
	DWORD exitCode = STILL_ACTIVE;
	bool forcedExitCode = false;
	bool exitCodeKnown = true;
	bool childProcess = true;
	bool waitable = true;
	bool nativeExitObserved = false;
	Pin<ProcessThreadObject> primaryThread;
	std::shared_future<void> primaryMonitor;

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
		DWORD ioBytes = 0;
		GUEST_PTR ioOverlapped = 0;
		bool ioCompletion = false;
	};
	std::deque<Entry> pending;
	bool terminated = false;
	ApcState() : WaitableObject(ObjectType::ApcQueue) {}
};

std::shared_ptr<ApcState> currentApcState();
void installApcState(std::shared_ptr<ApcState> state);
void closeApcState();
bool dispatchPendingApcs();
void queueIoCompletion(const std::shared_ptr<ApcState> &state, GUEST_PTR callback, DWORD error, DWORD bytes,
					   GUEST_PTR overlapped);
DWORD waitAlertable(HANDLE handle, WaitableObject *object, DWORD milliseconds);

bool createWorkerThread(DWORD (*function)(void *), void *parameter, DWORD &error);

struct ThreadObject final : WaitableObject {
	static constexpr ObjectType kType = ObjectType::Thread;

	pthread_t thread;
	DWORD threadId = 0;
	bool initialized = false;
	DWORD exitCode = STILL_ACTIVE;
	unsigned int suspendCount = 0;
	bool hostSuspended = false;
	WORD entryCs = 0;
	WORD entrySs = 0;
	TEB *tib = nullptr;
	bool ownsTib = true;
	std::shared_ptr<ApcState> apc = std::make_shared<ApcState>();
	// Protected by m; the counted UTF-16 description excludes its terminator.
	std::unique_ptr<WCHAR[]> description;
	size_t descriptionLength = 0;

	void onLastHandleClosed() noexcept override;

	explicit ThreadObject(pthread_t thread = pthread_null) : WaitableObject(kType), thread(thread) {}

	~ThreadObject() override {
		// Threads are detached at creation; we can safely drop
		if (tib && ownsTib) {
			wibo::destroyTib(tib);
			tib = nullptr;
		}
	}
};

void initializeMainThreadObject();
Pin<ThreadObject> currentThreadObject();
void captureThreadSelectors(ThreadObject &thread);

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
