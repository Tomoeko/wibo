#ifndef FIXTURE_DEPENDENCY_VALUE
#define FIXTURE_DEPENDENCY_VALUE 23
#endif

__declspec(dllexport) int dependency_value(void) { return FIXTURE_DEPENDENCY_VALUE; }
