#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef void(__cdecl *TranslatorFn)(unsigned int, EXCEPTION_POINTERS *);
typedef TranslatorFn(__cdecl *SetTranslatorFn)(TranslatorFn);

static volatile LONG calls;

static void __cdecl firstTranslator(unsigned int code, EXCEPTION_POINTERS *context) {
	(void)code;
	(void)context;
	InterlockedIncrement(&calls);
}

static void __cdecl secondTranslator(unsigned int code, EXCEPTION_POINTERS *context) {
	(void)code;
	(void)context;
	InterlockedIncrement(&calls);
}

struct ThreadCase {
	SetTranslatorFn set;
	LONG passed;
};

static DWORD WINAPI childThread(void *argument) {
	struct ThreadCase *test = argument;
	TEST_CHECK(test->set(NULL) == NULL);
	TEST_CHECK(test->set(secondTranslator) == NULL);
	TEST_CHECK(test->set(firstTranslator) == secondTranslator);
	TEST_CHECK(test->set(NULL) == firstTranslator);
	TEST_CHECK(test->set(secondTranslator) == NULL);
	InterlockedExchange(&test->passed, 1);
	return 0;
}

int main(void) {
	HMODULE runtime = LoadLibraryA("vcruntime140.dll");
	TEST_CHECK(runtime != NULL);
	FARPROC procedure = GetProcAddress(runtime, "_set_se_translator");
	TEST_CHECK(procedure != NULL);
	SetTranslatorFn set;
	TEST_CHECK(sizeof(set) == sizeof(procedure));
	memcpy(&set, &procedure, sizeof(set));
	TEST_CHECK(set(NULL) == NULL);
	TEST_CHECK(set(firstTranslator) == NULL);
	TEST_CHECK(set(firstTranslator) == firstTranslator);
	TEST_CHECK(set(secondTranslator) == firstTranslator);
	TEST_CHECK(set(NULL) == secondTranslator);
	TEST_CHECK(set(NULL) == NULL);
	TEST_CHECK(set(firstTranslator) == NULL);
	for (unsigned int iteration = 0; iteration < 3; ++iteration) {
		struct ThreadCase test = {set, 0};
		HANDLE child = CreateThread(NULL, 0, childThread, &test, 0, NULL);
		TEST_CHECK(child != NULL);
		TEST_CHECK_EQ(WaitForSingleObject(child, 5000), WAIT_OBJECT_0);
		TEST_CHECK(CloseHandle(child));
		TEST_CHECK_EQ(test.passed, 1);
		TEST_CHECK(set(firstTranslator) == firstTranslator);
	}
	TEST_CHECK(set(NULL) == firstTranslator);
	TEST_CHECK_EQ(calls, 0);
	TEST_CHECK(FreeLibrary(runtime));
	return 0;
}
