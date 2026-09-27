#include <windows.h>

#include "test_assert.h"

static const DWORD seed = 0x13579bdf;

static BYTE *read_information(HANDLE token, TOKEN_INFORMATION_CLASS kind, DWORD *size) {
	*size = seed;
	SetLastError(seed);
	TEST_CHECK(!GetTokenInformation(token, kind, NULL, 0, size));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK(*size > 0 && *size < 4096);
	DWORD capacity = *size;
	BYTE *buffer = malloc(capacity + 16);
	TEST_CHECK(buffer != NULL);
	memset(buffer, 0xa5, capacity + 16);
	*size = seed;
	SetLastError(seed);
	TEST_CHECK(GetTokenInformation(token, kind, buffer, capacity, size));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(capacity, *size);
	for (DWORD i = capacity; i < capacity + 16; ++i)
		TEST_CHECK_EQ(0xa5, buffer[i]);
	BYTE *shortBuffer = malloc(capacity + 16);
	TEST_CHECK(shortBuffer != NULL);
	memset(shortBuffer, 0xa5, capacity + 16);
	DWORD needed = seed;
	SetLastError(seed);
	TEST_CHECK(!GetTokenInformation(token, kind, shortBuffer, capacity - 1, &needed));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(capacity, needed);
	for (DWORD i = 0; i < capacity + 16; ++i)
		TEST_CHECK_EQ(0xa5, shortBuffer[i]);
	free(shortBuffer);
	return buffer;
}

static void check_sid(const BYTE *buffer, DWORD size, PSID sid, size_t offset) {
	TEST_CHECK((const BYTE *)sid == buffer + offset);
	TEST_CHECK(((SID *)sid)->Revision == SID_REVISION);
	TEST_CHECK(((SID *)sid)->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES);
	TEST_CHECK_EQ(offset + GetLengthSid(sid), size);
}

static void check_identity(HANDLE token, BYTE **userBuffer, DWORD *userSize, BYTE **groupBuffer, DWORD *groupSize,
						   DWORD *elevation) {
	*userBuffer = read_information(token, TokenUser, userSize);
	TOKEN_USER *user = (TOKEN_USER *)*userBuffer;
	check_sid(*userBuffer, *userSize, user->User.Sid, sizeof(*user));
	TEST_CHECK_EQ(0, user->User.Attributes);
	for (size_t i = sizeof(void *) + sizeof(DWORD); i < sizeof(TOKEN_USER); ++i)
		TEST_CHECK_EQ(0xa5, (*userBuffer)[i]);
	*groupBuffer = read_information(token, TokenPrimaryGroup, groupSize);
	TOKEN_PRIMARY_GROUP *group = (TOKEN_PRIMARY_GROUP *)*groupBuffer;
	check_sid(*groupBuffer, *groupSize, group->PrimaryGroup, sizeof(*group));
	DWORD size = 0;
	BYTE *bytes = read_information(token, TokenElevation, &size);
	TEST_CHECK_EQ(sizeof(TOKEN_ELEVATION), size);
	*elevation = ((TOKEN_ELEVATION *)bytes)->TokenIsElevated;
	TEST_CHECK(*elevation <= 1);
	free(bytes);
}

static void check_unavailable(HANDLE token, DWORD error) {
	const TOKEN_INFORMATION_CLASS kinds[] = {TokenUser, TokenPrimaryGroup, TokenElevation};
	for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
		BYTE buffer[256];
		memset(buffer, 0xa5, sizeof(buffer));
		DWORD needed = seed;
		SetLastError(seed);
		TEST_CHECK(!GetTokenInformation(token, kinds[i], buffer, sizeof(buffer), &needed));
		TEST_CHECK_EQ(error, GetLastError());
		TEST_CHECK_EQ(kinds[i] == TokenElevation ? 4 : 0, needed);
		for (unsigned j = 0; j < sizeof(buffer); ++j)
			TEST_CHECK_EQ(0xa5, buffer[j]);
	}
	DWORD type = 0, needed = 0;
	SetLastError(seed);
	TEST_CHECK(GetTokenInformation(token, TokenType, &type, sizeof(type), &needed));
	TEST_CHECK_EQ(seed, GetLastError());
	TEST_CHECK_EQ(TokenPrimary, type);
	TEST_CHECK_EQ(4, needed);
}

int main(void) {
	HANDLE token = NULL;
	TEST_CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token));
	const char *mode = getenv("WIBO_FIXTURE_TOKEN_IDENTITY_RESPONSE");
	if (mode && strcmp(mode, "unavailable") == 0) {
		check_unavailable(token, ERROR_NOT_SUPPORTED);
	} else if (mode && strcmp(mode, "native-error") == 0) {
		check_unavailable(token, ERROR_ACCESS_DENIED);
	} else if (mode && strcmp(mode, "good") != 0 && strcmp(mode, "restricted") != 0) {
		check_unavailable(token, ERROR_INVALID_DATA);
	} else {
		BYTE *user = NULL, *group = NULL;
		DWORD userSize = 0, groupSize = 0, elevation = 0;
		check_identity(token, &user, &userSize, &group, &groupSize, &elevation);
		HANDLE copy = NULL;
		TEST_CHECK(DuplicateTokenEx(token, TOKEN_QUERY, NULL, SecurityIdentification, TokenImpersonation, &copy));
		BYTE *copyUser = NULL, *copyGroup = NULL;
		DWORD copyUserSize = 0, copyGroupSize = 0, copyElevation = 0;
		check_identity(copy, &copyUser, &copyUserSize, &copyGroup, &copyGroupSize, &copyElevation);
		TEST_CHECK_EQ(userSize, copyUserSize);
		TEST_CHECK_EQ(groupSize, copyGroupSize);
		TEST_CHECK_EQ(elevation, copyElevation);
		TEST_CHECK(EqualSid(((TOKEN_USER *)user)->User.Sid, ((TOKEN_USER *)copyUser)->User.Sid));
		TEST_CHECK(
			EqualSid(((TOKEN_PRIMARY_GROUP *)group)->PrimaryGroup, ((TOKEN_PRIMARY_GROUP *)copyGroup)->PrimaryGroup));
		free(copyUser);
		free(copyGroup);
		free(user);
		free(group);
		TEST_CHECK(CloseHandle(copy));
	}
	TEST_CHECK(CloseHandle(token));
	return 0;
}
