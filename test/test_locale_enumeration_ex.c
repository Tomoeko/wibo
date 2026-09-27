#include "test_assert.h"
#include <stdint.h>
#include <windows.h>

struct enumeration {
	unsigned count, stop, hash;
	DWORD input;
	LPWSTR pointer;
	LPARAM cookie;
	int mutation, result, nested;
};
static struct enumeration *active;
static const DWORD seed = 0x13579bdf;
static struct enumeration run(DWORD, unsigned, int, int, int);

static LPARAM cookie(void) {
#if defined(_WIN64)
	return (LPARAM)(ULONG_PTR)UINT64_C(0xfedcba9876543210);
#else
	return (LPARAM)(ULONG_PTR)UINT32_C(0xfedcba98);
#endif
}

static BOOL CALLBACK record(LPWSTR text, DWORD flags, LPARAM parameter) {
	struct enumeration *state = active;
	TEST_CHECK_EQ(state->mutation && state->count ? 0x2468ace0u + state->count : seed, GetLastError());
	TEST_CHECK_U64_EQ((ULONG_PTR)state->cookie, (ULONG_PTR)parameter);
	TEST_CHECK(text != NULL);
	TEST_CHECK(!state->input || (flags & state->input));
	unsigned length = 0;
	while (length < LOCALE_NAME_MAX_LENGTH && text[length])
		++length;
	TEST_CHECK(length < LOCALE_NAME_MAX_LENGTH);
	WCHAR copy[LOCALE_NAME_MAX_LENGTH];
	memcpy(copy, text, (length + 1) * sizeof(WCHAR));
	if (!state->count)
		state->pointer = text;
	TEST_CHECK(state->pointer == text);
	for (unsigned i = 0; i <= length; ++i)
		state->hash = (state->hash ^ text[i]) * 16777619u;
	state->hash = (state->hash ^ flags) * 16777619u;
	++state->count;
	TEST_CHECK(state->count <= 10000);
	if (state->nested && state->count == 1) {
		const struct enumeration child = run(LOCALE_WINDOWS, 1, TRUE, 1, 0);
		TEST_CHECK_EQ(1, child.count);
		TEST_CHECK(child.pointer != text);
		TEST_CHECK(memcmp(copy, text, (length + 1) * sizeof(WCHAR)) == 0);
	}
	if (state->mutation) {
		text[0] = L'X';
		SetLastError(0x2468ace0u + state->count);
	} else
		SetLastError(seed);
	return state->stop && state->count >= state->stop ? FALSE : state->result;
}

static struct enumeration run(DWORD flags, unsigned stop, int result, int mutation, int nested) {
	struct enumeration state = {0};
	state.input = flags;
	state.stop = stop;
	state.result = result;
	state.mutation = mutation;
	state.nested = nested;
	state.hash = 2166136261u;
	state.cookie = cookie();
	struct enumeration *previous = active;
	active = &state;
	SetLastError(seed);
	const BOOL value = EnumSystemLocalesEx(record, flags, state.cookie, NULL);
	const DWORD error = GetLastError();
	active = previous;
	TEST_CHECK(value);
	TEST_CHECK_EQ(mutation && state.count ? 0x2468ace0u + state.count : seed, error);
	return state;
}

int main(void) {
	const char *mode = getenv("WIBO_FIXTURE_LOCALE_ENUMERATION_EX_RESPONSE");
	if (mode && strcmp(mode, "good") != 0) {
		DWORD error = ERROR_INVALID_DATA;
		if (strcmp(mode, "unavailable") == 0)
			error = ERROR_NOT_SUPPORTED;
		else if (strcmp(mode, "native-failure") == 0)
			error = ERROR_INVALID_FLAGS;
		struct enumeration state = {0};
		state.hash = 2166136261u;
		state.result = TRUE;
		state.cookie = cookie();
		active = &state;
		SetLastError(seed);
		const DWORD flags = strcmp(mode, "wrong-filter") == 0 ? LOCALE_WINDOWS : 0;
		TEST_CHECK(!EnumSystemLocalesEx(record, flags, state.cookie, NULL));
		TEST_CHECK_EQ(error, GetLastError());
		TEST_CHECK_EQ(0, state.count);
		active = NULL;
		return 0;
	}
	const DWORD flags[] = {0,
						   LOCALE_WINDOWS,
						   LOCALE_SUPPLEMENTAL,
						   LOCALE_ALTERNATE_SORTS,
						   LOCALE_NEUTRALDATA,
						   LOCALE_SPECIFICDATA,
						   LOCALE_WINDOWS | LOCALE_SUPPLEMENTAL,
						   LOCALE_NEUTRALDATA | LOCALE_SPECIFICDATA,
						   LOCALE_WINDOWS | LOCALE_NEUTRALDATA | LOCALE_SPECIFICDATA,
						   LOCALE_WINDOWS | LOCALE_ALTERNATE_SORTS | LOCALE_NEUTRALDATA | LOCALE_SPECIFICDATA};
	struct enumeration reference[sizeof(flags) / sizeof(flags[0])];
	for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
		reference[i] = run(flags[i], 0, TRUE, 0, 0);
		const struct enumeration repeated = run(flags[i], 0, 2, 1, 0);
		TEST_CHECK_EQ(reference[i].count, repeated.count);
		TEST_CHECK_EQ(reference[i].hash, repeated.hash);
	}
	TEST_CHECK(reference[0].count >= 3);
	const struct enumeration stop = run(0, 0, FALSE, 1, 0);
	TEST_CHECK_EQ(1, stop.count);
	const struct enumeration three = run(0, 3, TRUE, 1, 0);
	TEST_CHECK_EQ(3, three.count);
	const struct enumeration negative = run(0, 3, -1, 1, 0);
	TEST_CHECK_EQ(three.count, negative.count);
	TEST_CHECK_EQ(three.hash, negative.hash);
	const struct enumeration nested = run(0, 3, TRUE, 1, 1);
	TEST_CHECK_EQ(three.count, nested.count);
	TEST_CHECK_EQ(three.hash, nested.hash);

	struct enumeration state = {0};
	DWORD reserved = 0xa5a5a5a5u;
	active = &state;
	SetLastError(seed);
	TEST_CHECK(!EnumSystemLocalesEx(record, 0, cookie(), &reserved));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(0, state.count);
	TEST_CHECK_EQ(0xa5a5a5a5u, reserved);
	active = NULL;
	return 0;
}
