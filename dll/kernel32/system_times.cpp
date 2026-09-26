#include "processthreadsapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "timeutil.h"

#include <cstdint>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/processor_info.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace kernel32 {

BOOL WINAPI GetSystemTimes(FILETIME *idleTime, FILETIME *kernelTime, FILETIME *userTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTimes(%p, %p, %p)\n", idleTime, kernelTime, userTime);
	const long frequency = sysconf(_SC_CLK_TCK);
	if (frequency <= 0) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	uint64_t idle = 0, kernel = 0, user = 0;
#ifdef __APPLE__
	natural_t processors = 0;
	processor_info_array_t information = nullptr;
	mach_msg_type_number_t count = 0;
	const mach_port_t host = mach_host_self();
	const kern_return_t status = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &processors, &information, &count);
	mach_port_deallocate(mach_task_self(), host);
	if (status != KERN_SUCCESS) {
		setLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	const bool valid = processors > 0 && processors <= 64 && count / PROCESSOR_CPU_LOAD_INFO_COUNT >= processors;
	if (valid) {
		const auto *loads = reinterpret_cast<const processor_cpu_load_info_data_t *>(information);
		for (natural_t i = 0; i < processors; ++i) {
			idle += loads[i].cpu_ticks[CPU_STATE_IDLE];
			kernel += loads[i].cpu_ticks[CPU_STATE_SYSTEM];
			user += static_cast<uint64_t>(loads[i].cpu_ticks[CPU_STATE_USER]) + loads[i].cpu_ticks[CPU_STATE_NICE];
		}
	}
	vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(information), count * sizeof(integer_t));
	if (!valid) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
#elif defined(__linux__)
	if (sysconf(_SC_NPROCESSORS_ONLN) > 64) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::ifstream statistics("/proc/stat");
	std::string name;
	uint64_t nice = 0, waiting = 0, irq = 0, softIrq = 0;
	if (!(statistics >> name >> user >> nice >> kernel >> idle >> waiting >> irq >> softIrq) || name != "cpu") {
		setLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	user += nice;
	idle += waiting;
	kernel += irq + softIrq;
#else
	setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
#endif
	kernel += idle;
	const auto convert = [frequency](uint64_t ticks) {
		return fileTimeFromDuration(
			static_cast<uint64_t>(static_cast<__int128>(ticks) * HUNDRED_NS_PER_SECOND / frequency));
	};
	if (idleTime)
		*idleTime = convert(idle);
	if (kernelTime)
		*kernelTime = convert(kernel);
	if (userTime)
		*userTime = convert(user);
	return TRUE;
}

} // namespace kernel32
