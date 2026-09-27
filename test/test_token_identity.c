#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "test_assert.h"

struct GuardedBuffer {
	DWORD before;
	BYTE value[128];
	DWORD after;
};

struct SyntheticSid {
	BYTE revision;
	BYTE count;
	SID_IDENTIFIER_AUTHORITY authority;
	DWORD sub[2];
};

static const struct SyntheticSid userSid = {1, 1, {{0, 0, 0, 0, 0, 5}}, {12345678, 0}};
static const struct SyntheticSid worldSid = {1, 1, {{0, 0, 0, 0, 0, 1}}, {0, 0}};
static const struct SyntheticSid adminSid = {1, 2, {{0, 0, 0, 0, 0, 5}}, {32, 544}};
static const struct SyntheticSid usersSid = {1, 2, {{0, 0, 0, 0, 0, 5}}, {32, 545}};
static const struct SyntheticSid absentSid = {1, 1, {{0, 0, 0, 0, 0, 5}}, {12345679, 0}};

static void checkGuard(const struct GuardedBuffer *buffer) {
	TEST_CHECK_EQ(0x13579bdf, buffer->before);
	TEST_CHECK_EQ(0x2468ace0, buffer->after);
}

static void initialize(struct GuardedBuffer *buffer) {
	buffer->before = 0x13579bdf;
	memset(buffer->value, 0xa5, sizeof(buffer->value));
	buffer->after = 0x2468ace0;
}

static void checkFailure(HANDLE token, TOKEN_INFORMATION_CLASS kind, DWORD error, DWORD neededValue) {
	struct GuardedBuffer buffer;
	initialize(&buffer);
	DWORD needed = 0xabababab;
	SetLastError(4321);
	TEST_CHECK(!GetTokenInformation(token, kind, buffer.value, sizeof(buffer.value), &needed));
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_EQ(neededValue, needed);
	for (size_t index = 0; index < sizeof(buffer.value); ++index)
		TEST_CHECK_EQ(0xa5, buffer.value[index]);
	checkGuard(&buffer);
}

static void checkIdentity(HANDLE token) {
	struct GuardedBuffer buffer;
	initialize(&buffer);
	DWORD needed = 0;
	SetLastError(4321);
	TEST_CHECK(GetTokenInformation(token, TokenUser, buffer.value, sizeof(buffer.value), &needed));
	TEST_CHECK_EQ(4321, GetLastError());
	TEST_CHECK_EQ(sizeof(TOKEN_USER) + 12, needed);
	TOKEN_USER user;
	memcpy(&user, buffer.value, sizeof(user));
	TEST_CHECK(user.User.Sid == buffer.value + sizeof(TOKEN_USER));
	TEST_CHECK_EQ(0, user.User.Attributes);
	TEST_CHECK(memcmp(user.User.Sid, &userSid, 12) == 0);
	for (size_t index = sizeof(void *) + sizeof(DWORD); index < sizeof(TOKEN_USER); ++index)
		TEST_CHECK_EQ(0xa5, buffer.value[index]);
	for (size_t index = needed; index < sizeof(buffer.value); ++index)
		TEST_CHECK_EQ(0xa5, buffer.value[index]);
	checkGuard(&buffer);
	initialize(&buffer);
	SetLastError(4321);
	TEST_CHECK(GetTokenInformation(token, TokenPrimaryGroup, buffer.value, sizeof(buffer.value), &needed));
	TEST_CHECK_EQ(4321, GetLastError());
	TEST_CHECK_EQ(sizeof(TOKEN_PRIMARY_GROUP) + 12, needed);
	TOKEN_PRIMARY_GROUP primary;
	memcpy(&primary, buffer.value, sizeof(primary));
	TEST_CHECK(primary.PrimaryGroup == buffer.value + sizeof(TOKEN_PRIMARY_GROUP));
	TEST_CHECK(memcmp(primary.PrimaryGroup, &worldSid, 12) == 0);
	for (size_t index = needed; index < sizeof(buffer.value); ++index)
		TEST_CHECK_EQ(0xa5, buffer.value[index]);
	checkGuard(&buffer);
	initialize(&buffer);
	SetLastError(4321);
	TEST_CHECK(GetTokenInformation(token, TokenElevation, buffer.value, sizeof(buffer.value), &needed));
	TEST_CHECK_EQ(4321, GetLastError());
	TEST_CHECK_EQ(4, needed);
	DWORD elevation;
	memcpy(&elevation, buffer.value, sizeof(elevation));
	TEST_CHECK_EQ(1, elevation);
	for (size_t index = needed; index < sizeof(buffer.value); ++index)
		TEST_CHECK_EQ(0xa5, buffer.value[index]);
	checkGuard(&buffer);
}

static void checkMembership(HANDLE token, const struct SyntheticSid *sid, BOOL expected, DWORD error) {
	struct {
		DWORD before;
		BOOL member;
		DWORD after;
	} output = {0x13579bdf, 0x5a5a5a5a, 0x2468ace0};
	SetLastError(4321);
	BOOL result = CheckTokenMembership(token, (PSID)sid, &output.member);
	TEST_CHECK_EQ(error == 0, result != FALSE);
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_EQ(expected, output.member);
	TEST_CHECK_EQ(0x13579bdf, output.before);
	TEST_CHECK_EQ(0x2468ace0, output.after);
}

int main(int argc, char **argv) {
	TEST_CHECK_EQ(2, argc);
	HANDLE token = NULL;
	TEST_CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token));
	const char *mode = argv[1];
	const BOOL native = !strncmp(mode, "native-evaluated", strlen("native-evaluated"));
	if (!strcmp(mode, "success") || !strcmp(mode, "restricted") || !strcmp(mode, "retry") || native) {
		if (!strcmp(mode, "retry"))
			checkFailure(token, TokenUser, ERROR_INVALID_DATA, 0);
		checkIdentity(token);
		checkMembership(token, &worldSid, FALSE, ERROR_NO_IMPERSONATION_TOKEN);
		checkMembership((HANDLE)(ULONG_PTR)0x7fff0004, &worldSid, FALSE, ERROR_INVALID_HANDLE);
		HANDLE noQuery = NULL;
		TEST_CHECK(
			DuplicateTokenEx(token, TOKEN_DUPLICATE, NULL, SecurityIdentification, TokenImpersonation, &noQuery));
		checkMembership(noQuery, &worldSid, FALSE, ERROR_ACCESS_DENIED);
		TEST_CHECK(CloseHandle(noQuery));
		HANDLE duplicate = NULL;
		TEST_CHECK(DuplicateTokenEx(token, TOKEN_QUERY, NULL, SecurityIdentification, TokenImpersonation, &duplicate));
		TEST_CHECK(CloseHandle(token));
		token = NULL;
		checkIdentity(duplicate);
		DWORD error = !strcmp(mode, "restricted") ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
		checkMembership(duplicate, &userSid, error || native ? FALSE : TRUE, error);
		checkMembership(duplicate, &worldSid, error ? FALSE : TRUE, error);
		checkMembership(duplicate, &adminSid, FALSE, error);
		checkMembership(duplicate, &usersSid, FALSE, error);
		checkMembership(duplicate, &absentSid, FALSE, error);
		checkMembership(NULL, &worldSid, error ? FALSE : TRUE, error);
		struct SyntheticSid invalidSid = userSid;
		invalidSid.revision = 2;
		checkMembership(duplicate, &invalidSid, FALSE, ERROR_INVALID_SID);
		TEST_CHECK(CloseHandle(duplicate));
	} else {
		DWORD error = !strcmp(mode, "failed")											 ? ERROR_ACCESS_DENIED
					  : !strcmp(mode, "unavailable") || !strcmp(mode, "user-attributes") ? ERROR_NOT_SUPPORTED
																						 : ERROR_INVALID_DATA;
		checkFailure(token, TokenUser, error, 0);
		checkFailure(token, TokenElevation, error, 4);
		TEST_CHECK(CloseHandle(token));
	}
	puts("token identity snapshot checks passed");
	return 0;
}
