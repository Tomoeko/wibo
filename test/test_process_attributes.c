#include "test_assert.h"
#include <windows.h>

int main(void) {
	SIZE_T size = 0;
	TEST_CHECK(!InitializeProcThreadAttributeList(NULL, 1, 0, &size));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK(size > 0);
	LPPROC_THREAD_ATTRIBUTE_LIST list = HeapAlloc(GetProcessHeap(), 0, size);
	TEST_CHECK(list != NULL);
	SIZE_T too_small = size - 1;
	TEST_CHECK(!InitializeProcThreadAttributeList(list, 1, 0, &too_small));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(size, too_small);
	TEST_CHECK(InitializeProcThreadAttributeList(list, 1, 0, &size));
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	HANDLE event = CreateEventA(&security, FALSE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	TEST_CHECK(
		UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &event, sizeof(event), NULL, NULL));
	TEST_CHECK(
		!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &event, sizeof(event), NULL, NULL));
	DeleteProcThreadAttributeList(list);
	TEST_CHECK(HeapFree(GetProcessHeap(), 0, list));
	TEST_CHECK(CloseHandle(event));
	return 0;
}
