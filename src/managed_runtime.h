#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wibo {

// Replaces the standalone host process with an explicitly configured CLR runner.
int execManagedProgram(const std::filesystem::path &program, const std::vector<std::string> &arguments);

} // namespace wibo
