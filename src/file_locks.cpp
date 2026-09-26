#include "files.h"

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

} // namespace

DWORD lockRange(FileObject *file, uint64_t start, uint64_t length, bool exclusive, bool blocking) {
	if (!length || start > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
		length > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - start) {
		return ERROR_NOT_SUPPORTED;
	}
	const uint64_t end = start + length;
	std::lock_guard guard(file->m);
	for (const auto &range : file->rangeLocks) {
		if (overlaps(start, end, range)) {
			if (exclusive) {
				return ERROR_LOCK_VIOLATION;
			}
			if (range.exclusive) {
				// Mixed overlapping locks need separate shared and exclusive ownership accounting.
				return ERROR_NOT_SUPPORTED;
			}
		}
	}
	const DWORD error = hostRangeLock(file, start, end, exclusive ? F_WRLCK : F_RDLCK, blocking);
	if (!error) {
		file->rangeLocks.push_back({start, end, exclusive});
	}
	return error;
}

DWORD unlockRange(FileObject *file, uint64_t start, uint64_t length) {
	std::lock_guard guard(file->m);
	if (!length || length > std::numeric_limits<uint64_t>::max() - start) {
		return ERROR_NOT_LOCKED;
	}
	const uint64_t end = start + length;
	auto found = std::find_if(file->rangeLocks.begin(), file->rangeLocks.end(),
							  [&](const auto &range) { return range.start == start && range.end == end; });
	if (found == file->rangeLocks.end()) {
		return ERROR_NOT_LOCKED;
	}
	// Release only portions that no remaining registration covers. Repeated shared locks retain ownership.
	std::vector<uint64_t> boundaries{start, end};
	for (auto it = file->rangeLocks.begin(); it != file->rangeLocks.end(); ++it) {
		if (it != found && overlaps(start, end, *it)) {
			boundaries.push_back(std::max(start, it->start));
			boundaries.push_back(std::min(end, it->end));
		}
	}
	std::sort(boundaries.begin(), boundaries.end());
	boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
	for (size_t i = 1; i < boundaries.size(); ++i) {
		bool covered = false;
		for (auto it = file->rangeLocks.begin(); it != file->rangeLocks.end(); ++it) {
			if (it != found && overlaps(boundaries[i - 1], boundaries[i], *it)) {
				covered = true;
				break;
			}
		}
		if (!covered) {
			const DWORD error = hostRangeLock(file, boundaries[i - 1], boundaries[i], F_UNLCK, false);
			if (error) {
				return error;
			}
		}
	}
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
