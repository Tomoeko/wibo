#include <stdlib.h>
#include <windows.h>

int main(void) {
	ULONG_PTR thread_name_info[] = {0x1000, (ULONG_PTR)"wibo test worker", (ULONG_PTR)-1, 0};
	RaiseException(0x406D1388, 0, 4, thread_name_info);
	return EXIT_SUCCESS;
}
