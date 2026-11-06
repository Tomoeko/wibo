#include "managed_runtime.h"

#include "common.h"
#include "diagnostics.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace wibo {

int execManagedProgram(const std::filesystem::path &program, const std::vector<std::string> &arguments) {
	const char *runtime = std::getenv("WIBO_MANAGED_RUNTIME");
	if (!runtime || runtime[0] == '\0') {
		diagnosticLog("Managed executable requires a CLR runner; set WIBO_MANAGED_RUNTIME to its executable path\n");
		return 1;
	}
	const std::string programPath = program.string();
	std::vector<char *> runtimeArguments;
	runtimeArguments.reserve(arguments.size() + 3);
	runtimeArguments.push_back(const_cast<char *>(runtime));
	runtimeArguments.push_back(const_cast<char *>(programPath.c_str()));
	for (size_t index = 1; index < arguments.size(); ++index) {
		runtimeArguments.push_back(const_cast<char *>(arguments[index].c_str()));
	}
	runtimeArguments.push_back(nullptr);
	DEBUG_LOG("Delegating managed executable to configured CLR runner\n");
	std::fflush(nullptr);
	execvp(runtime, runtimeArguments.data());
	diagnosticLog("Failed to start configured CLR runner: %s\n", std::strerror(errno));
	return 127;
}

} // namespace wibo
