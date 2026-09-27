#ifndef FIXTURE_DEPENDENCY_NAME
#define FIXTURE_DEPENDENCY_NAME dependency_value
#endif
#ifndef FIXTURE_EXPORT_NAME
#define FIXTURE_EXPORT_NAME parent_value
#endif
#ifndef FIXTURE_VALUE_ADJUSTMENT
#define FIXTURE_VALUE_ADJUSTMENT 1
#endif

__declspec(dllimport) int FIXTURE_DEPENDENCY_NAME(void);

__declspec(dllexport) int FIXTURE_EXPORT_NAME(void) { return FIXTURE_DEPENDENCY_NAME() + FIXTURE_VALUE_ADJUSTMENT; }
