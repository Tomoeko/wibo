#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef float(__cdecl *ParseFloatFn)(const char *text, char **end);

static uint32_t float_bits(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static void check_float(ParseFloatFn parse, const char *text, uint32_t expected_bits, size_t expected_end) {
	char *end = NULL;
	float value = parse(text, &end);
	TEST_CHECK_EQ(expected_bits, float_bits(value));
	TEST_CHECK_EQ(expected_end, (size_t)(end - text));
}

int main(void) {
	HMODULE conversion = LoadLibraryA("api-ms-win-crt-convert-l1-1-0.dll");
	TEST_CHECK(conversion != NULL);
	FARPROC procedure = GetProcAddress(conversion, "strtof");
	TEST_CHECK(procedure != NULL);
	TEST_CHECK(sizeof(ParseFloatFn) == sizeof(procedure));
	ParseFloatFn parse;
	memcpy(&parse, &procedure, sizeof(parse));
	check_float(parse, "  -12.375xyz", 0xc1460000u, 9);
	check_float(parse, "1.23456789", 0x3f9e0652u, 10);
	check_float(parse, "0x1.000001p0", 0x3f800000u, 12);
	check_float(parse, "bad", 0, 0);
	TEST_CHECK(GetProcAddress(conversion, "_atoflt") != NULL);
	TEST_CHECK(FreeLibrary(conversion));

	HMODULE console = LoadLibraryA("api-ms-win-crt-conio-l1-1-0.dll");
	TEST_CHECK(console != NULL);
	TEST_CHECK(GetProcAddress(console, "__conio_common_vcprintf") != NULL);
	TEST_CHECK(GetProcAddress(console, "__conio_common_vcwprintf") != NULL);
	TEST_CHECK(FreeLibrary(console));
	return 0;
}
