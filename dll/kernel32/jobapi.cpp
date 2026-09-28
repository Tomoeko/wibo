#include "jobapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "processthreadsapi.h"

#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kJobObjectAssignProcess = 0x0001;
constexpr DWORD kJobObjectSetAttributes = 0x0002;
constexpr DWORD kJobObjectQuery = 0x0004;
constexpr DWORD kProcessSetQuota = 0x0100;
constexpr DWORD kJobObjectAllAccess = 0x001f003f;
constexpr DWORD kJobObjectExtendedLimitInformation = 9;
constexpr DWORD kJobObjectBasicLimitInformation = 2;
constexpr DWORD kJobObjectLimitKillOnJobClose = 0x00002000;
constexpr size_t kMaxNameUnits = 32767;

struct JobBasicLimits {
	LONGLONG perProcessUserTimeLimit;
	LONGLONG perJobUserTimeLimit;
	DWORD limitFlags;
	SIZE_T minimumWorkingSetSize;
	SIZE_T maximumWorkingSetSize;
	DWORD activeProcessLimit;
	ULONG_PTR affinity;
	DWORD priorityClass;
	DWORD schedulingClass;
};

struct JobIoCounters {
	ULONGLONG readOperationCount;
	ULONGLONG writeOperationCount;
	ULONGLONG otherOperationCount;
	ULONGLONG readTransferCount;
	ULONGLONG writeTransferCount;
	ULONGLONG otherTransferCount;
};

struct JobExtendedLimits {
	JobBasicLimits basic;
	JobIoCounters io;
	SIZE_T processMemoryLimit;
	SIZE_T jobMemoryLimit;
	SIZE_T peakProcessMemoryUsed;
	SIZE_T peakJobMemoryUsed;
};

#ifdef WIBO_GUEST_64
static_assert(sizeof(JobExtendedLimits) == 144);
#else
static_assert(sizeof(JobExtendedLimits) == 112);
#endif

std::mutex g_jobMutex;

bool readName(LPCWSTR source, std::u16string &name) {
	if (!source)
		return true;
	size_t length = 0;
	while (length < kMaxNameUnits && source[length])
		++length;
	if (length == kMaxNameUnits) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	name.assign(reinterpret_cast<const char16_t *>(source), length);
	if (name.find(u'\\') != std::u16string::npos) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	return true;
}

bool readName(LPCSTR source, std::u16string &name) {
	if (!source)
		return true;
	for (size_t length = 0; length < kMaxNameUnits; ++length) {
		const unsigned char character = static_cast<unsigned char>(source[length]);
		if (!character)
			return true;
		if (character >= 0x80 || character == '\\') {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		name.push_back(static_cast<char16_t>(character));
	}
	kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return false;
}

} // namespace

namespace kernel32 {

struct JobObject final : ObjectBase {
	static constexpr ObjectType kType = ObjectType::Job;
	bool killOnClose = false;
	bool closed = false;
	std::vector<Pin<ProcessObject>> members;

	JobObject() : ObjectBase(kType) {}
	void onLastHandleClosed() noexcept override {
		std::vector<Pin<ProcessObject>> assigned;
		bool terminate = false;
		{
			std::lock_guard lock(g_jobMutex);
			closed = true;
			terminate = killOnClose;
			assigned.swap(members);
			for (const auto &process : assigned)
				process->job = nullptr;
		}
		if (terminate) {
			for (const auto &process : assigned) {
				std::lock_guard lock(process->m);
				if (process->signaled || process->nativeExitObserved || process->pid <= 0)
					continue;
#ifdef __linux__
				if (process->pidfd >= 0)
					syscall(SYS_pidfd_send_signal, process->pidfd, SIGKILL, nullptr, 0);
				else
#endif
					kill(process->pid, SIGKILL);
			}
		}
	}
};

} // namespace kernel32

namespace {

HANDLE createJobObject(LPSECURITY_ATTRIBUTES attributes, const std::u16string &name) {
	if (attributes && attributes->lpSecurityDescriptor) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	auto [job, created] = wibo::g_namespace.getOrCreate(name, [] { return new kernel32::JobObject(); });
	if (!job) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	const DWORD flags = attributes && attributes->bInheritHandle ? HANDLE_FLAG_INHERIT : 0;
	const HANDLE handle = wibo::handles().alloc(std::move(job), kJobObjectAllAccess, flags);
	kernel32::setLastError(created ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS);
	return handle;
}

} // namespace

namespace kernel32 {

HANDLE WINAPI CreateJobObjectA(LPSECURITY_ATTRIBUTES attributes, LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateJobObjectA(%p, %p)\n", attributes, name);
	std::u16string wideName;
	if (!readName(name, wideName))
		return NO_HANDLE;
	return createJobObject(attributes, wideName);
}

HANDLE WINAPI CreateJobObjectW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateJobObjectW(%p, %p)\n", attributes, name);
	std::u16string wideName;
	if (!readName(name, wideName))
		return NO_HANDLE;
	return createJobObject(attributes, wideName);
}

BOOL WINAPI SetInformationJobObject(HANDLE jobHandle, DWORD informationClass, LPVOID information, DWORD length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetInformationJobObject(%p, %u, %p, %u)\n", jobHandle, informationClass, information, length);
	HandleMeta metadata{};
	auto job = wibo::handles().getAs<JobObject>(jobHandle, &metadata);
	if (!job) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & kJobObjectSetAttributes)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (informationClass != kJobObjectExtendedLimitInformation) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!information || length != sizeof(JobExtendedLimits)) {
		setLastError(ERROR_BAD_LENGTH);
		return FALSE;
	}
	const auto *limits = static_cast<const JobExtendedLimits *>(information);
	if (limits->basic.limitFlags & ~kJobObjectLimitKillOnJobClose) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	{
		std::lock_guard lock(g_jobMutex);
		if (job->closed) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		job->killOnClose = (limits->basic.limitFlags & kJobObjectLimitKillOnJobClose) != 0;
	}
	return TRUE;
}

BOOL WINAPI QueryInformationJobObject(HANDLE jobHandle, DWORD informationClass, LPVOID information, DWORD length,
									  LPDWORD returnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("QueryInformationJobObject(%p, %u, %p, %u, %p)\n", jobHandle, informationClass, information, length,
			  returnLength);
	HandleMeta metadata{};
	auto job = wibo::handles().getAs<JobObject>(jobHandle, &metadata);
	if (!job) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & kJobObjectQuery)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (informationClass != kJobObjectBasicLimitInformation && informationClass != kJobObjectExtendedLimitInformation) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const DWORD required =
		informationClass == kJobObjectBasicLimitInformation ? sizeof(JobBasicLimits) : sizeof(JobExtendedLimits);
	if (!information || length < required) {
		setLastError(ERROR_BAD_LENGTH);
		return FALSE;
	}
	JobExtendedLimits limits{};
	{
		std::lock_guard lock(g_jobMutex);
		if (job->closed) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (informationClass == kJobObjectExtendedLimitInformation && !job->members.empty()) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
		if (job->killOnClose)
			limits.basic.limitFlags |= kJobObjectLimitKillOnJobClose;
	}
	std::memcpy(information, &limits, required);
	if (returnLength)
		*returnLength = required;
	return TRUE;
}

BOOL WINAPI AssignProcessToJobObject(HANDLE jobHandle, HANDLE processHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AssignProcessToJobObject(%p, %p)\n", jobHandle, processHandle);
	HandleMeta jobMetadata{};
	auto job = wibo::handles().getAs<JobObject>(jobHandle, &jobMetadata);
	if (!job) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(jobMetadata.grantedAccess & kJobObjectAssignProcess)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (isPseudoCurrentProcessHandle(processHandle)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	HandleMeta processMetadata{};
	auto process = wibo::handles().getAs<ProcessObject>(processHandle, &processMetadata);
	if (!process) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if ((processMetadata.grantedAccess & (kProcessSetQuota | PROCESS_TERMINATE)) !=
		(kProcessSetQuota | PROCESS_TERMINATE)) {
		setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (process->pid == getpid() || !process->childProcess) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	{
		std::lock_guard lock(g_jobMutex);
		if (job->closed) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (process->assignedToJob) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		process->job = job.get();
		process->assignedToJob = true;
		job->members.push_back(std::move(process));
	}
	return TRUE;
}

BOOL WINAPI IsProcessInJob(HANDLE processHandle, HANDLE jobHandle, BOOL *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsProcessInJob(%p, %p, %p)\n", processHandle, jobHandle, result);
	if (!result) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	Pin<ProcessObject> process;
	if (!isPseudoCurrentProcessHandle(processHandle)) {
		HandleMeta metadata{};
		process = wibo::handles().getAs<ProcessObject>(processHandle, &metadata);
		if (!process) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		if (!(metadata.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION))) {
			setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
		if (process->pid != getpid() && !process->childProcess) {
			setLastError(ERROR_NOT_SUPPORTED);
			return FALSE;
		}
	}
	Pin<JobObject> job;
	if (jobHandle != NO_HANDLE) {
		job = wibo::handles().getAs<JobObject>(jobHandle);
		if (!job) {
			setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
	}
	if (!process) {
		*result = FALSE;
		return TRUE;
	}
	std::lock_guard lock(g_jobMutex);
	*result = job ? process->job == job.get() : process->assignedToJob;
	return TRUE;
}

} // namespace kernel32
