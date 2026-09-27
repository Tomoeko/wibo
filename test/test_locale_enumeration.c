#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

struct enumeration {
	unsigned count, stop, hash;
	uintptr_t pointer;
	DWORD error;
	int mutation, return_value, nested;
};
static struct enumeration *active;
static const DWORD seed = 0x13579bdf;
static struct enumeration run(unsigned api, DWORD flags, unsigned stop, int mutation, int result, int nested);

static BOOL record(const char *text, void *pointer, int wide) {
	struct enumeration *state = active;
	TEST_CHECK_EQ(state->count && state->mutation ? 0x2468ace0 + state->count : seed, GetLastError());
	TEST_CHECK_EQ(8, strlen(text));
	for (unsigned i = 0; i < 8; ++i) {
		const unsigned c = (unsigned char)text[i];
		TEST_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
		state->hash = (state->hash ^ c) * 16777619u;
	}
	if (!state->count)
		state->pointer = (uintptr_t)pointer;
	TEST_CHECK_U64_EQ(state->pointer, (uintptr_t)pointer);
	++state->count;
	if (state->nested && state->count == 1) {
		const struct enumeration child = run(1, LCID_SUPPORTED, 1, 1, TRUE, 0);
		TEST_CHECK_EQ(1, child.count);
		TEST_CHECK(child.pointer != (uintptr_t)pointer);
		for (unsigned i = 0; i < 9; ++i)
			TEST_CHECK_EQ((unsigned char)text[i], wide ? ((WCHAR *)pointer)[i] : ((unsigned char *)pointer)[i]);
	}
	if (state->mutation) {
		if (wide)
			((WCHAR *)pointer)[0] = L'X';
		else
			((char *)pointer)[0] = 'X';
		SetLastError(0x2468ace0 + state->count);
	} else
		SetLastError(seed);
	return state->stop && state->count >= state->stop ? FALSE : state->return_value;
}
static BOOL CALLBACK callback_a(LPSTR text) {
	char copy[9];
	memcpy(copy, text, 9);
	return record(copy, text, 0);
}
static BOOL CALLBACK callback_w(LPWSTR text) {
	char copy[9];
	for (unsigned i = 0; i < 9; ++i)
		copy[i] = (char)text[i];
	return record(copy, text, 1);
}
static struct enumeration run(unsigned api, DWORD flags, unsigned stop, int mutation, int result, int nested) {
	struct enumeration state = {0};
	state.stop = stop;
	state.hash = 2166136261u;
	state.mutation = mutation;
	state.return_value = result;
	state.nested = nested;
	struct enumeration *previous = active;
	active = &state;
	SetLastError(seed);
	const BOOL value = api ? EnumSystemLocalesW(callback_w, flags) : EnumSystemLocalesA(callback_a, flags);
	state.error = GetLastError();
	active = previous;
	TEST_CHECK(value);
	TEST_CHECK_EQ(state.count && mutation ? 0x2468ace0 + state.count : seed, state.error);
	return state;
}
int main(void) {
	const char *mode = getenv("WIBO_FIXTURE_LOCALE_ENUMERATION_RESPONSE");
	if (mode && strcmp(mode, "good") != 0) {
		DWORD error = ERROR_INVALID_DATA;
		if (strcmp(mode, "unavailable") == 0)
			error = ERROR_NOT_SUPPORTED;
		else if (strcmp(mode, "native-failure") == 0)
			error = ERROR_INVALID_FLAGS;
		struct enumeration state = {0};
		state.hash = 2166136261u;
		state.return_value = TRUE;
		active = &state;
		for (unsigned api = 0; api < 2; ++api) {
			SetLastError(seed);
			TEST_CHECK(!(api ? EnumSystemLocalesW(callback_w, LCID_SUPPORTED)
							 : EnumSystemLocalesA(callback_a, LCID_SUPPORTED)));
			TEST_CHECK_EQ(error, GetLastError());
			TEST_CHECK_EQ(0, state.count);
		}
		active = NULL;
		return 0;
	}
	const DWORD flags[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0xffffffffu};
	struct enumeration reference[10];
	for (unsigned api = 0; api < 2; ++api) {
		for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
			const struct enumeration value = run(api, flags[i], 0, 0, TRUE, 0);
			if (!api)
				reference[i] = value;
			else {
				TEST_CHECK_EQ(reference[i].count, value.count);
				TEST_CHECK_EQ(reference[i].hash, value.hash);
			}
		}
		TEST_CHECK(reference[2].count >= 3);
		TEST_CHECK_EQ(reference[0].count, reference[2].count);
		TEST_CHECK_EQ(reference[0].hash, reference[2].hash);
		TEST_CHECK_EQ(reference[1].count, reference[2].count);
		TEST_CHECK_EQ(reference[1].hash, reference[2].hash);
		TEST_CHECK_EQ(reference[3].count, reference[2].count);
		TEST_CHECK_EQ(reference[3].hash, reference[2].hash);
		TEST_CHECK_EQ(reference[2].count + reference[4].count, reference[6].count);
		TEST_CHECK_EQ(0, reference[8].count);
		const struct enumeration stop = run(api, 0, 1, 0, TRUE, 0);
		TEST_CHECK_EQ(1, stop.count);
		const struct enumeration three = run(api, 2, 3, 1, TRUE, 0);
		TEST_CHECK_EQ(3, three.count);
		const struct enumeration positive = run(api, 2, 0, 1, 2, 0);
		TEST_CHECK_EQ(reference[2].count, positive.count);
		TEST_CHECK_EQ(reference[2].hash, positive.hash);
		const struct enumeration negative = run(api, 2, 0, 1, -1, 0);
		TEST_CHECK_EQ(reference[2].count, negative.count);
		TEST_CHECK_EQ(reference[2].hash, negative.hash);
		const struct enumeration nested = run(api, 2, 3, 1, TRUE, 1);
		TEST_CHECK_EQ(3, nested.count);
		TEST_CHECK_EQ(three.hash, nested.hash);
	}
	return 0;
}
