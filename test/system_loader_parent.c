#ifndef FIXTURE_DEPENDENCY_NAME
#define FIXTURE_DEPENDENCY_NAME dependency_value
#endif

__declspec(dllimport) int FIXTURE_DEPENDENCY_NAME(void);

__declspec(dllexport) int parent_value(void) { return FIXTURE_DEPENDENCY_NAME() + 1; }
