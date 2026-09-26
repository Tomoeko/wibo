#include "ntdll.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/internal.h"
#include "system_provider.h"

#include <cstring>

namespace ntdll {
NTSTATUS WINAPI NtQueryVolumeInformationFile(HANDLE file, PIO_STATUS_BLOCK ioStatus, PVOID information, ULONG length,
											 ULONG informationClass) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryVolumeInformationFile(%p, %p, %p, %u, %u)\n", file, ioStatus, information, length,
			  informationClass);
	if (!ioStatus)
		return STATUS_ACCESS_VIOLATION;
	auto complete = [&](NTSTATUS status, ULONG_PTR used = 0) {
		ioStatus->Status = status;
		ioStatus->Information = used;
		return status;
	};
	auto object = wibo::handles().getAs<kernel32::FsObject>(file);
	if (!object || !object->valid())
		return complete(STATUS_INVALID_HANDLE);
	if (length && !information)
		return complete(STATUS_ACCESS_VIOLATION);
	// These structures contain scalar fields and inline UTF-16 data in either guest ABI.
	switch (informationClass) {
	case 1:	 // FileFsVolumeInformation
	case 3:	 // FileFsSizeInformation
	case 4:	 // FileFsDeviceInformation
	case 5:	 // FileFsAttributeInformation
	case 7:	 // FileFsFullSizeInformation
	case 11: // FileFsSectorSizeInformation
		break;
	default:
		return complete(STATUS_NOT_SUPPORTED);
	}
	if (length > 1024 * 1024)
		return complete(STATUS_NOT_SUPPORTED);
	std::filesystem::path path;
	{
		std::lock_guard lock(object->m);
		path = object->canonicalPath;
	}
	if (path.empty())
		return complete(STATUS_NOT_SUPPORTED);
	std::vector<uint8_t> response;
	if (!wibo::provider::request(
			{"volume-query", files::pathToWindows(path), std::to_string(informationClass), std::to_string(length)},
			response))
		return complete(STATUS_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t error = 0;
	if (!reader.header(error))
		return complete(STATUS_UNEXPECTED_IO_ERROR);
	if (error)
		return complete(reader.done() ? wibo::statusFromWinError(error) : STATUS_UNEXPECTED_IO_ERROR);
	uint32_t status = 0, used = 0, high = 0;
	std::vector<uint8_t> data;
	if (!reader.number(status) || !reader.number(used) || !reader.number(high) || !reader.bytes(data) ||
		!reader.done() || high || used > length || used != data.size())
		return complete(STATUS_UNEXPECTED_IO_ERROR);
	if (!data.empty())
		memcpy(information, data.data(), data.size());
	return complete(static_cast<NTSTATUS>(status), used);
}
} // namespace ntdll
