#pragma once

#include "fileapi.h"

#include <cerrno>
#include <cstdint>
#include <sys/stat.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

namespace kernel32::fileAttributes {

constexpr uint8_t kStoredReadOnly = 1;
constexpr uint8_t kStoredHidden = 2;
#if defined(__APPLE__)
constexpr char kStoredAttributesName[] = "com.wibo.attributes";
#elif defined(__linux__)
constexpr char kStoredAttributesName[] = "user.wibo.attributes";
#endif

inline bool missingStoredAttributes(int error) {
#if defined(__APPLE__)
	return error == ENOATTR || error == ENOTSUP;
#elif defined(__linux__)
	return error == ENODATA || error == ENOTSUP;
#else
	return error == ENOTSUP;
#endif
}

inline bool readStoredAttributes(const char *path, int fd, uint8_t &bits) {
	bits = 0;
	if (!path && fd < 0)
		return true;
	ssize_t size = -1;
#if defined(__APPLE__)
	size = fd >= 0 ? fgetxattr(fd, kStoredAttributesName, &bits, sizeof(bits), 0, 0)
					   : getxattr(path, kStoredAttributesName, &bits, sizeof(bits), 0, 0);
	if (size < 0 && fd >= 0 && errno == EBADF && path)
		size = getxattr(path, kStoredAttributesName, &bits, sizeof(bits), 0, 0);
#elif defined(__linux__)
	size = fd >= 0 ? fgetxattr(fd, kStoredAttributesName, &bits, sizeof(bits))
					   : getxattr(path, kStoredAttributesName, &bits, sizeof(bits));
	if (size < 0 && fd >= 0 && errno == EBADF && path)
		size = getxattr(path, kStoredAttributesName, &bits, sizeof(bits));
#else
	errno = ENOTSUP;
	return false;
#endif
	if (size < 0 && missingStoredAttributes(errno)) {
		bits = 0;
		return true;
	}
	if (size < 0)
		return false;
	if (size != sizeof(bits) || bits & ~(kStoredReadOnly | kStoredHidden)) {
		errno = EINVAL;
		return false;
	}
	return true;
}

inline bool writeStoredAttributes(const char *path, uint8_t bits) {
	int result = -1;
#if defined(__APPLE__)
	result = bits ? setxattr(path, kStoredAttributesName, &bits, sizeof(bits), 0, 0)
				  : removexattr(path, kStoredAttributesName, 0);
#elif defined(__linux__)
	result = bits ? setxattr(path, kStoredAttributesName, &bits, sizeof(bits), 0)
				  : removexattr(path, kStoredAttributesName);
#else
	(void)path;
	(void)bits;
	errno = ENOTSUP;
	return false;
#endif
	return result == 0 || (!bits && missingStoredAttributes(errno));
}

inline bool buildFileAttributes(const struct stat &st, bool isDirectory, DWORD &attributes,
								const char *path = nullptr, int fd = -1) {
	attributes = 0;
	uint8_t stored = 0;
	if (!readStoredAttributes(path, fd, stored))
		return false;
	if (S_ISDIR(st.st_mode) || isDirectory)
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	if (S_ISREG(st.st_mode) && !isDirectory)
		attributes |= FILE_ATTRIBUTE_ARCHIVE;
	if (!isDirectory && ((st.st_mode & S_IWUSR) == 0 || (stored & kStoredReadOnly)))
		attributes |= FILE_ATTRIBUTE_READONLY;
	if (stored & kStoredHidden)
		attributes |= FILE_ATTRIBUTE_HIDDEN;
#if defined(__APPLE__)
	if (st.st_flags & UF_HIDDEN)
		attributes |= FILE_ATTRIBUTE_HIDDEN;
#endif
	if (attributes == 0)
		attributes = FILE_ATTRIBUTE_NORMAL;
	return true;
}

} // namespace kernel32::fileAttributes
