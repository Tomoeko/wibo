#define COBJMACROS
#include <objbase.h>
#include <windows.h>

#include "test_assert.h"

static ULONG_PTR mainToken;
static IUnknown *retainedContext;
static const IID objectContextId = {0x1c6, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

static void check_interfaces(ULONG_PTR token) {
	IUnknown *object = (IUnknown *)token;
	IUnknown *result = NULL;
	TEST_CHECK_EQ(S_OK, IUnknown_QueryInterface(object, &IID_IUnknown, (void **)&result));
	IUnknown *identity = NULL;
	TEST_CHECK_EQ(S_OK, IUnknown_QueryInterface(result, &IID_IUnknown, (void **)&identity));
	TEST_CHECK(identity == result);
	TEST_CHECK_EQ(1, IUnknown_Release(identity));
	TEST_CHECK_EQ(2, IUnknown_AddRef(result));
	TEST_CHECK_EQ(1, IUnknown_Release(result));
	TEST_CHECK_EQ(0, IUnknown_Release(result));
	TEST_CHECK_EQ(S_OK, IUnknown_QueryInterface(object, &objectContextId, (void **)&result));
	TEST_CHECK(result == object);
	TEST_CHECK_EQ(0, IUnknown_Release(result));
	const IID unsupported = {0x761de821, 0xd827, 0x4e18, {0x83, 0x14, 0x73, 0x33, 0x32, 0x18, 0x4b, 0x90}};
	result = (IUnknown *)(ULONG_PTR)0x1234;
	TEST_CHECK_EQ(E_NOINTERFACE, IUnknown_QueryInterface(object, &unsupported, (void **)&result));
	TEST_CHECK(result == NULL);
}

static DWORD WINAPI worker(LPVOID parameter) {
	(void)parameter;
	ULONG_PTR token = 0;
	TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
	TEST_CHECK(token != 0 && token != mainToken);
	ULONG_PTR workerToken = token;
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
	TEST_CHECK(token == workerToken);
	CoUninitialize();
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
	TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
	TEST_CHECK(token == workerToken);
	check_interfaces(token);
	CoUninitialize();
	TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
	TEST_CHECK(token == workerToken);
	return 0;
}

static DWORD WINAPI retained_worker(LPVOID parameter) {
	(void)parameter;
	TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, COINIT_MULTITHREADED));
	ULONG_PTR token = 0;
	TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
	TEST_CHECK_EQ(S_OK, IUnknown_QueryInterface((IUnknown *)token, &IID_IUnknown, (void **)&retainedContext));
	return 0;
}

int main(void) {
	ULONG_PTR token = 0x1234;
	SetLastError(0x4123);
	TEST_CHECK_EQ(CO_E_NOTINITIALIZED, CoGetContextToken(NULL));
	TEST_CHECK_EQ(0x4123, GetLastError());
	TEST_CHECK_EQ(CO_E_NOTINITIALIZED, CoGetContextToken(&token));
	TEST_CHECK_EQ(0x1234, token);
	TEST_CHECK_EQ(0x4123, GetLastError());
	for (unsigned i = 0; i < 2; ++i) {
		DWORD mode = i == 0 ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED;
		TEST_CHECK_EQ(S_OK, CoInitializeEx(NULL, mode));
		TEST_CHECK_EQ(E_POINTER, CoGetContextToken(NULL));
		TEST_CHECK_EQ(S_OK, CoGetContextToken(&mainToken));
		TEST_CHECK(mainToken != 0);
		TEST_CHECK_EQ(S_FALSE, CoInitializeEx(NULL, mode));
		TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
		TEST_CHECK(token == mainToken);
		check_interfaces(token);
		CoUninitialize();
		TEST_CHECK_EQ(S_OK, CoGetContextToken(&token));
		TEST_CHECK(token == mainToken);
		if (i == 1) {
			HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
			TEST_CHECK(thread != NULL);
			TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
			DWORD code = 1;
			TEST_CHECK(GetExitCodeThread(thread, &code));
			TEST_CHECK_EQ(0, code);
			TEST_CHECK(CloseHandle(thread));
		}
		CoUninitialize();
		token = 0x1234;
		TEST_CHECK_EQ(CO_E_NOTINITIALIZED, CoGetContextToken(&token));
		TEST_CHECK_EQ(0x1234, token);
	}
	HANDLE thread = CreateThread(NULL, 0, retained_worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK_EQ(CO_E_NOTINITIALIZED, CoGetContextToken(&token));
	TEST_CHECK(retainedContext != NULL);
	ULONG references = IUnknown_AddRef(retainedContext);
	TEST_CHECK(references != 0);
	TEST_CHECK_EQ(references - 1, IUnknown_Release(retainedContext));
	IUnknown_Release(retainedContext);
	return 0;
}
