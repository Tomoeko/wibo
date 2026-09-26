#include "directory_changes.h"

#include "context.h"
#include "errors.h"
#include "fileapi.h"
#include "overlapped_util.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace kernel32 {
namespace {
constexpr DWORD kNameFilters = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
constexpr DWORD kDataFilters = FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr DWORD kMetadataFilters = FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_CREATION;
constexpr DWORD kUnavailableFilters = FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_SECURITY;
constexpr DWORD kValidFilters = kNameFilters | kDataFilters | kMetadataFilters | kUnavailableFilters;
struct Notification {
	DWORD action;
	std::u16string name;
	[[nodiscard]] size_t size() const { return (12 + name.size() * 2 + 3) & ~size_t{3}; }
};
struct DirectoryRequest {
	void *buffer;
	DWORD length;
	OVERLAPPED *operation;
	GUEST_PTR callback;
	std::shared_ptr<ApcState> apc;
	bool done = false;
	NTSTATUS status = STATUS_PENDING;
	DWORD bytes = 0;
	pthread_t owner = pthread_self();
};
std::mutex g_watchersMutex;
struct RegisteredWatcher {
	Pin<DirectoryObject> directory;
	std::weak_ptr<DirectoryWatcher> watcher;
};
std::vector<RegisteredWatcher> g_watchers;
} // namespace

class DirectoryWatcher {
	DirectoryObject &directory;
	std::deque<Notification> notifications;
	std::deque<std::shared_ptr<DirectoryRequest>> requests;
	size_t bufferedBytes = 0;
	const DWORD capacity;
	DWORD filter;
	bool subtree;
	bool overflow = false;
	std::atomic<bool> armed{false};
#ifdef __APPLE__
	FSEventStreamRef stream = nullptr;
	dispatch_queue_t queue = nullptr;
	static void receive(ConstFSEventStreamRef, void *context, size_t count, void *paths,
						const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
		auto &self = *static_cast<DirectoryWatcher *>(context);
		if (!self.armed.load(std::memory_order_acquire))
			return;
		std::lock_guard lock(self.directory.m);
		if (self.directory.watchClosed)
			return;
		for (size_t i = 0; i < count; ++i)
			self.receivePath(static_cast<char **>(paths)[i], flags[i]);
		self.deliver();
	}
	void receivePath(std::string_view path, FSEventStreamEventFlags flags) {
		DEBUG_LOG("directory notification: %.*s flags=0x%x\n", static_cast<int>(path.size()), path.data(), flags);
		constexpr auto lost = kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
							  kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagEventIdsWrapped |
							  kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount |
							  kFSEventStreamEventFlagUnmount;
		if (flags & lost) {
			loseDetails();
			return;
		}
		const std::string root = directory.canonicalPath.string();
		if (!path.starts_with(root) || path.size() <= root.size() || path[root.size()] != '/')
			return;
		path.remove_prefix(root.size() + 1);
		if (!subtree && path.find('/') != std::string_view::npos)
			return;
		const bool directoryEntry = flags & kFSEventStreamEventFlagItemIsDir;
		const bool nameChange = filter & (directoryEntry ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME);
		const bool created = flags & kFSEventStreamEventFlagItemCreated;
		const bool removed = flags & kFSEventStreamEventFlagItemRemoved;
		// Coalesced renames do not identify both endpoints or their order.
		if (nameChange && ((flags & kFSEventStreamEventFlagItemRenamed) || (created && removed))) {
			loseDetails();
			return;
		}
		DWORD action = 0;
		if (nameChange && created)
			action = FILE_ACTION_ADDED;
		else if (nameChange && removed)
			action = FILE_ACTION_REMOVED;
		else if ((filter & FILE_NOTIFY_CHANGE_LAST_WRITE) && (flags & kFSEventStreamEventFlagItemModified))
			action = FILE_ACTION_MODIFIED;
		else if (((filter & kMetadataFilters) &&
				  (flags & (kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagItemFinderInfoMod |
							kFSEventStreamEventFlagItemChangeOwner))) ||
				 ((filter & FILE_NOTIFY_CHANGE_SIZE) && (flags & kFSEventStreamEventFlagItemModified))) {
			// These flags do not distinguish the requested metadata or size change.
			loseDetails();
			return;
		}
		if (!action)
			return;
		auto wide = utf8ToUtf16(path);
		if (!wide) {
			loseDetails();
			return;
		}
		std::replace(wide->begin(), wide->end(), u'/', u'\\');
		Notification notification{action, std::move(*wide)};
		if (notification.size() > capacity - bufferedBytes) {
			loseDetails();
			return;
		}
		if (!overflow) {
			bufferedBytes += notification.size();
			notifications.push_back(std::move(notification));
		}
	}
#endif
	void loseDetails() {
		notifications.clear();
		bufferedBytes = 0;
		overflow = true;
	}
	void finish(const std::shared_ptr<DirectoryRequest> &request, NTSTATUS status, DWORD bytes) {
		request->status = status;
		request->bytes = bytes;
		request->done = true;
		if (request->operation) {
			if (request->callback) {
				__atomic_store_n(&request->operation->InternalHigh, static_cast<ULONG_PTR>(bytes), __ATOMIC_RELAXED);
				__atomic_store_n(&request->operation->Internal, static_cast<ULONG_PTR>(status), __ATOMIC_RELEASE);
				const DWORD error = wibo::winErrorFromNtStatus(status);
				queueIoCompletion(request->apc, request->callback, error, bytes, toGuestPtr(request->operation));
			} else
				detail::signalOverlappedEvent(&directory, request->operation, status, bytes);
		}
		directory.overlappedCv.notify_all();
	}
	void deliver() {
		if (requests.empty() || (!overflow && notifications.empty()))
			return;
		auto request = requests.front();
		requests.pop_front();
		DWORD bytes = 0;
		if (!overflow && bufferedBytes <= request->length) {
			auto *out = static_cast<uint8_t *>(request->buffer);
			for (size_t i = 0; i < notifications.size(); ++i) {
				const auto &notification = notifications[i];
				const DWORD recordSize = static_cast<DWORD>(notification.size());
				const DWORD header[] = {i + 1 == notifications.size() ? 0U : recordSize, notification.action,
										static_cast<DWORD>(notification.name.size() * 2)};
				std::memset(out + bytes, 0, recordSize);
				std::memcpy(out + bytes, header, sizeof(header));
				// NOLINTNEXTLINE(bugprone-not-null-terminated-result): File names are byte-counted.
				std::memcpy(out + bytes + sizeof(header), notification.name.data(), notification.name.size() * 2);
				bytes += recordSize;
			}
		}
		notifications.clear();
		bufferedBytes = 0;
		overflow = false;
		finish(request, STATUS_SUCCESS, bytes);
	}

  public:
	DirectoryWatcher(DirectoryObject &directory, DWORD capacity, DWORD filter, bool subtree)
		: directory(directory), capacity(capacity), filter(filter), subtree(subtree) {}
	bool start() {
#ifdef __APPLE__
		const auto root = directory.canonicalPath.string();
		CFStringRef path = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(root.data()),
												   static_cast<CFIndex>(root.size()), kCFStringEncodingUTF8, false);
		if (!path)
			return false;
		CFArrayRef paths =
			CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void **>(&path), 1, &kCFTypeArrayCallBacks);
		FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
		stream = FSEventStreamCreate(
			kCFAllocatorDefault, &receive, &context, paths, kFSEventStreamEventIdSinceNow, 0.01,
			kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
		CFRelease(paths);
		CFRelease(path);
		if (!stream)
			return false;
		queue = dispatch_queue_create("directory.notifications", DISPATCH_QUEUE_SERIAL);
		FSEventStreamSetDispatchQueue(stream, queue);
		if (!FSEventStreamStart(stream))
			return false;
		// Drain events queued before registration, then begin observing new changes.
		FSEventStreamFlushSync(stream);
		armed.store(true, std::memory_order_release);
		return true;
#else
		return false;
#endif
	}
	~DirectoryWatcher() {
		armed.store(false, std::memory_order_release);
#ifdef __APPLE__
		if (stream) {
			FSEventStreamStop(stream);
			FSEventStreamInvalidate(stream);
			// Drain callbacks before releasing their directory context.
			dispatch_sync_f(queue, nullptr, [](void *) {});
			FSEventStreamRelease(stream);
		}
		if (queue)
			dispatch_release(queue);
#endif
	}
	void close() {
		for (const auto &request : requests)
			finish(request, STATUS_HANDLES_CLOSED, 0);
		requests.clear();
	}
	void cancelThread(pthread_t thread) {
		std::lock_guard lock(directory.m);
		if (std::atomic_load(&directory.completion))
			return;
		for (auto it = requests.begin(); it != requests.end();) {
			if (pthread_equal((*it)->owner, thread)) {
				finish(*it, STATUS_CANCELLED, 0);
				it = requests.erase(it);
			} else
				++it;
		}
	}
	bool cancel(OVERLAPPED *operation) {
		bool found = false;
		for (auto it = requests.begin(); it != requests.end();) {
			if (!operation || (*it)->operation == operation) {
				finish(*it, STATUS_CANCELLED, 0);
				it = requests.erase(it);
				found = true;
			} else
				++it;
		}
		return found;
	}
	bool enqueue(const std::shared_ptr<DirectoryRequest> &request, DWORD newFilter, bool newSubtree) {
		if (request->operation && std::any_of(requests.begin(), requests.end(), [&](const auto &pending) {
				return pending->operation == request->operation;
			}))
			return false;
		filter = newFilter;
		subtree = newSubtree;
		if (request->operation) {
			__atomic_store_n(&request->operation->InternalHigh, ULONG_PTR{0}, __ATOMIC_RELAXED);
			__atomic_store_n(&request->operation->Internal, static_cast<ULONG_PTR>(STATUS_PENDING), __ATOMIC_RELEASE);
			if (!request->callback)
				detail::resetOverlappedEvent(request->operation);
		}
		requests.push_back(request);
		deliver();
		return true;
	}
};

DirectoryObject::~DirectoryObject() { onLastHandleClosed(); }
void DirectoryObject::onLastHandleClosed() noexcept {
	std::shared_ptr<DirectoryWatcher> old;
	{
		std::lock_guard lock(m);
		watchClosed = true;
		old = std::move(watcher);
		if (old)
			old->close();
	}
	{
		std::lock_guard registryLock(g_watchersMutex);
		std::erase_if(g_watchers, [&](const auto &entry) { return entry.directory.get() == this; });
	}
	old.reset();
}
bool cancelDirectoryChanges(DirectoryObject &directory, OVERLAPPED *operation) {
	std::lock_guard lock(directory.m);
	return directory.watcher && directory.watcher->cancel(operation);
}

void cancelDirectoryIoForThread(pthread_t thread) {
	struct ActiveWatcher {
		Pin<DirectoryObject> directory;
		std::shared_ptr<DirectoryWatcher> watcher;
	};
	std::vector<ActiveWatcher> watchers;
	{
		std::lock_guard lock(g_watchersMutex);
		for (auto it = g_watchers.begin(); it != g_watchers.end();) {
			if (auto watcher = it->watcher.lock()) {
				watchers.push_back({it->directory.clone(), std::move(watcher)});
				++it;
			} else
				it = g_watchers.erase(it);
		}
	}
	for (const auto &watcher : watchers)
		watcher.watcher->cancelThread(thread);
}

BOOL WINAPI ReadDirectoryChangesW(HANDLE handle, LPVOID buffer, DWORD length, BOOL subtree, DWORD filter,
								  LPDWORD returned, LPOVERLAPPED operation, LPOVERLAPPED_COMPLETION_ROUTINE callback) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReadDirectoryChangesW(%p, %p, %u, %d, 0x%x, %p, %p, %p)\n", handle, buffer, length, subtree, filter,
			  returned, operation, callback);
	HandleMeta metadata{};
	auto directory = wibo::handles().getAs<DirectoryObject>(handle, &metadata);
	if (!directory || !directory->valid()) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & 1)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (!buffer || (reinterpret_cast<uintptr_t>(buffer) & 3)) {
		setLastError(ERROR_NOACCESS);
		return FALSE;
	}
	if (!length || !filter || (filter & ~kValidFilters) || (directory->overlapped && !operation) ||
		(callback && (!operation || !directory->overlapped)) || (!operation && !returned)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (filter & kUnavailableFilters) {
		// The native stream does not report reads or Windows security descriptors.
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	auto request = std::make_shared<DirectoryRequest>(DirectoryRequest{buffer, length, operation,
																	   toGuestPtr(reinterpret_cast<void *>(callback)),
																	   callback ? currentApcState() : nullptr});
	std::unique_lock lock(directory->m);
	if (directory->watchClosed) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (callback && std::atomic_load(&directory->completion)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!directory->watcher) {
		auto watcher = std::make_shared<DirectoryWatcher>(*directory, length, filter, subtree != FALSE);
		if (!watcher->start()) {
			lock.unlock();
			watcher.reset();
			setLastError(1); // ERROR_INVALID_FUNCTION
			return FALSE;
		}
		{
			std::lock_guard registryLock(g_watchersMutex);
			g_watchers.push_back({directory.clone(), watcher});
		}
		directory->watcher = std::move(watcher);
	}
	if (!directory->watcher->enqueue(request, filter, subtree != FALSE)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!operation) {
		CompletionWait wait;
		directory->overlappedCv.wait(lock, [&] { return request->done; });
		*returned = request->bytes;
		if (request->status != STATUS_SUCCESS) {
			setLastError(wibo::winErrorFromNtStatus(request->status));
			return FALSE;
		}
	}
	return TRUE;
}
} // namespace kernel32
