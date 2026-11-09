#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

struct ExceptionData {
	const char *what;
	bool doFree;
};

typedef void(__cdecl *CopyFn)(const struct ExceptionData *, struct ExceptionData *);
typedef void(__cdecl *DestroyFn)(struct ExceptionData *);

struct GuardedData {
	uintptr_t before;
	struct ExceptionData data;
	uintptr_t after;
};

int main(void) {
	HMODULE runtime = LoadLibraryA("vcruntime140.dll");
	TEST_CHECK(runtime != NULL);
	FARPROC procedure = GetProcAddress(runtime, "__std_exception_copy");
	TEST_CHECK(procedure != NULL);
	CopyFn copy;
	TEST_CHECK(sizeof(copy) == sizeof(procedure));
	memcpy(&copy, &procedure, sizeof(copy));
	procedure = GetProcAddress(runtime, "__std_exception_destroy");
	TEST_CHECK(procedure != NULL);
	DestroyFn destroy;
	TEST_CHECK(sizeof(destroy) == sizeof(procedure));
	memcpy(&destroy, &procedure, sizeof(destroy));

	for (unsigned int owned = 0; owned < 2; ++owned) {
		for (unsigned int kind = 0; kind < 4; ++kind) {
			char text[] = "mutable exception message\x80\xff";
			const char *message = kind == 0 ? NULL : kind == 1 ? "" : kind == 2 ? "literal message" : text;
			struct ExceptionData source = {message, owned != 0};
			const struct ExceptionData expectedSource = source;
			struct GuardedData output = {0x34567891, {NULL, false}, 0x76543219};
			copy(&source, &output.data);
			TEST_CHECK(source.what == expectedSource.what && source.doFree == expectedSource.doFree);
			TEST_CHECK_EQ(output.before, 0x34567891);
			TEST_CHECK_EQ(output.after, 0x76543219);
			if (message == NULL) {
				TEST_CHECK(output.data.what == NULL && !output.data.doFree);
			} else {
				TEST_CHECK(output.data.what != NULL && strcmp(output.data.what, message) == 0);
				TEST_CHECK(output.data.doFree == (owned != 0));
				TEST_CHECK((output.data.what == message) == (owned == 0));
				if (kind == 3) {
					text[0] = 'X';
					TEST_CHECK_EQ(output.data.what[0], owned ? 'm' : 'X');
				}
			}
			destroy(&output.data);
			TEST_CHECK(output.data.what == NULL && !output.data.doFree);
			destroy(&output.data);
			TEST_CHECK(output.data.what == NULL && !output.data.doFree);
			TEST_CHECK(source.what == expectedSource.what && source.doFree == expectedSource.doFree);
			TEST_CHECK_EQ(output.before, 0x34567891);
			TEST_CHECK_EQ(output.after, 0x76543219);
		}
	}
	struct ExceptionData source = {"independent owned copies", true};
	struct ExceptionData first = {NULL, false};
	struct ExceptionData second = {NULL, false};
	copy(&source, &first);
	copy(&first, &second);
	TEST_CHECK(first.what != NULL && second.what != NULL);
	TEST_CHECK(first.what != source.what && second.what != source.what && second.what != first.what);
	TEST_CHECK(first.doFree && second.doFree);
	destroy(&first);
	TEST_CHECK(strcmp(second.what, source.what) == 0);
	destroy(&second);
	TEST_CHECK(FreeLibrary(runtime));
	return 0;
}
