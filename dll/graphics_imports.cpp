#include "modules.h"

// Compiler-only DLLs can import graphics entry points without calling them.
// Keep those imports on the loader's fail-on-call path; no graphics API is implemented.
extern const wibo::ModuleStub lib_d3d9 = {
	(const char *[]){"d3d9", nullptr},
	nullptr,
	nullptr,
};

extern const wibo::ModuleStub lib_gdi32 = {
	(const char *[]){"gdi32", nullptr},
	nullptr,
	nullptr,
};
