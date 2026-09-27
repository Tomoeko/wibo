#include "files.h"

#include "common.h"
#include "errors.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>

namespace files {

namespace {

bool overlaps(uint64_t start, uint64_t end, const kernel32::FileRangeLock &range) {
	return start < range.end && range.start < end;
}

DWORD hostRangeLock(FileObject *file, uint64_t start, uint64_t end, short type, bool blocking) {
#if defined(F_OFD_SETLK) && defined(F_OFD_SETLKW)
	struct flock request{};
	request.l_type = type;
	request.l_whence = SEEK_SET;
	request.l_start = static_cast<off_t>(start);
	request.l_len = static_cast<off_t>(end - start);
	int result;
	do {
		result = fcntl(file->fd, blocking ? F_OFD_SETLKW : F_OFD_SETLK, &request);
	} while (result == -1 && errno == EINTR);
	if (result == 0) {
		return ERROR_SUCCESS;
	}
	if (errno == EACCES || errno == EAGAIN) {
		return ERROR_LOCK_VIOLATION;
	}
	return wibo::winErrorFromErrno(errno);
#else
	(void)file;
	(void)start;
	(void)end;
	(void)type;
	(void)blocking;
	// Process-scoped POSIX locks cannot distinguish separately opened guest handles.
	return ERROR_NOT_SUPPORTED;
#endif
}

short rangeMode(const std::vector<kernel32::FileRangeLock> &locks, uint64_t start, uint64_t end,
				const kernel32::FileRangeLock *excluded = nullptr) {
	short mode = F_UNLCK;
	for (const auto &range : locks) {
		if (&range != excluded && overlaps(start, end, range)) {
			if (range.exclusive)
				return F_WRLCK;
			mode = F_RDLCK;
		}
	}
	return mode;
}

DWORD changeRangeLocks(FileObject *file, uint64_t start, uint64_t end, const kernel32::FileRangeLock *added,
					   const kernel32::FileRangeLock *removed, bool blocking) {
	std::vector<uint64_t> boundaries{start, end};
	for (const auto &range : file->rangeLocks) {
		if (overlaps(start, end, range)) {
			boundaries.push_back(std::max(start, range.start));
			boundaries.push_back(std::min(end, range.end));
		}
	}
	std::sort(boundaries.begin(), boundaries.end());
	boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
	struct Change {
		uint64_t start, end;
		short before, after;
	};
	std::vector<Change> changes;
	for (size_t i = 1; i < boundaries.size(); ++i) {
		const auto begin = boundaries[i - 1], finish = boundaries[i];
		const short before = rangeMode(file->rangeLocks, begin, finish);
		short after = rangeMode(file->rangeLocks, begin, finish, removed);
		if (added && overlaps(begin, finish, *added)) {
			if (added->exclusive)
				after = F_WRLCK;
			else if (after == F_UNLCK)
				after = F_RDLCK;
		}
		if (before != after)
			changes.push_back({begin, finish, before, after});
	}
	for (size_t i = 0; i < changes.size(); ++i) {
		const auto &change = changes[i];
		const DWORD error = hostRangeLock(file, change.start, change.end, change.after, blocking);
		if (error) {
			// Restore native ownership before leaving the registrations unchanged.
			while (i > 0) {
				const auto &previous = changes[--i];
				const DWORD restoreError = hostRangeLock(file, previous.start, previous.end, previous.before, false);
				if (restoreError)
					DEBUG_LOG("Range lock restoration failed: error=%u\n", restoreError);
			}
			return error;
		}
	}
	return ERROR_SUCCESS;
}

} // namespace

DWORD lockRange(FileObject *file, uint64_t start, uint64_t length, bool exclusive, bool blocking) {
	if (!length || start > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
		length > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - start) {
		return ERROR_NOT_SUPPORTED;
	}
	const uint64_t end = start + length;
	std::lock_guard guard(file->m);
	if (file->cursor.controlDescriptorLocked() >= 0)
		return ERROR_NOT_SUPPORTED;
	for (const auto &range : file->rangeLocks) {
		if (exclusive && overlaps(start, end, range))
			return ERROR_LOCK_VIOLATION;
	}
	const kernel32::FileRangeLock added{start, end, exclusive};
	const DWORD error = changeRangeLocks(file, start, end, &added, nullptr, blocking);
	if (!error)
		file->rangeLocks.push_back(added);
	return error;
}

DWORD unlockRange(FileObject *file, uint64_t start, uint64_t length) {
	std::lock_guard guard(file->m);
	if (file->cursor.controlDescriptorLocked() >= 0)
		return ERROR_NOT_SUPPORTED;
	if (!length || length > std::numeric_limits<uint64_t>::max() - start) {
		return ERROR_NOT_LOCKED;
	}
	const uint64_t end = start + length;
	auto found = std::find_if(file->rangeLocks.begin(), file->rangeLocks.end(),
							  [&](const auto &range) { return range.start == start && range.end == end; });
	if (found == file->rangeLocks.end()) {
		return ERROR_NOT_LOCKED;
	}
	const DWORD error = changeRangeLocks(file, start, end, nullptr, &*found, false);
	if (error)
		return error;
	file->rangeLocks.erase(found);
	return ERROR_SUCCESS;
}

DWORD checkRangeAccess(FileObject *file, off_t start, size_t length, bool writing) {
	// The caller holds the file mutex while checking ownership and performing I/O.
	if (start < 0 || !length || length > static_cast<uint64_t>(std::numeric_limits<off_t>::max() - start)) {
		return ERROR_INVALID_PARAMETER;
	}
	if (writing) {
		for (const auto &range : file->rangeLocks) {
			if (!range.exclusive && overlaps(start, static_cast<uint64_t>(start) + length, range)) {
				return ERROR_LOCK_VIOLATION;
			}
		}
	}
#if defined(F_OFD_GETLK)
	struct flock request{};
	request.l_type = writing ? F_WRLCK : F_RDLCK;
	request.l_whence = SEEK_SET;
	request.l_start = start;
	request.l_len = static_cast<off_t>(length);
	if (fcntl(file->fd, F_OFD_GETLK, &request) == -1) {
		return wibo::winErrorFromErrno(errno);
	}
	return request.l_type == F_UNLCK ? ERROR_SUCCESS : ERROR_LOCK_VIOLATION;
#else
	return ERROR_SUCCESS;
#endif
}

} // namespace files
