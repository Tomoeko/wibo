__declspec(dllimport) int dependency_value(void);
__declspec(dllimport) int missing_value(void);

__declspec(dllexport) int failure_value(void) { return dependency_value() + missing_value(); }
