#include "test_assert.h"
#include <windows.h>

int main(void) {
	char name[256];
	WCHAR wideName[256];
	DWORD characters = sizeof(name);
	TEST_CHECK(GetUserNameA(name, &characters));
	TEST_CHECK_EQ(strlen(name) + 1, characters);
	characters = sizeof(wideName) / sizeof(wideName[0]);
	TEST_CHECK(GetUserNameW(wideName, &characters));
	TEST_CHECK_EQ(wcslen(wideName) + 1, characters);
	DWORD sidBytes = 0, domainCharacters = 0;
	SID_NAME_USE use = SidTypeUnknown;
	TEST_CHECK(!LookupAccountNameA(NULL, name, NULL, &sidBytes, NULL, &domainCharacters, &use));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK(sidBytes >= 8 && sidBytes <= SECURITY_MAX_SID_SIZE);
	TEST_CHECK(domainCharacters > 0 && domainCharacters <= 256);
	BYTE sid[SECURITY_MAX_SID_SIZE];
	char domain[256];
	DWORD capacity = sizeof(sid), domainCapacity = sizeof(domain);
	SetLastError(0x1234);
	TEST_CHECK(LookupAccountNameA(NULL, name, sid, &capacity, domain, &domainCapacity, &use));
	if (getenv("WIBO_FIXTURE_PROVIDER"))
		TEST_CHECK_EQ(0x1234, GetLastError());
	TEST_CHECK_EQ(SidTypeUser, use);
	TEST_CHECK_EQ(sidBytes, capacity);
	TEST_CHECK_EQ(strlen(domain), domainCapacity);
	TEST_CHECK_EQ(domainCharacters, domainCapacity + 1);
	TEST_CHECK_EQ(SID_REVISION, sid[0]);
	TEST_CHECK_EQ(8 + 4 * sid[1], sidBytes);
	WCHAR wideDomain[256];
	BYTE wideSid[SECURITY_MAX_SID_SIZE];
	capacity = sizeof(wideSid);
	domainCapacity = sizeof(wideDomain) / sizeof(wideDomain[0]);
	TEST_CHECK(LookupAccountNameW(NULL, wideName, wideSid, &capacity, wideDomain, &domainCapacity, &use));
	TEST_CHECK(memcmp(sid, wideSid, sidBytes) == 0);
	TEST_CHECK_EQ(wcslen(wideDomain), domainCapacity);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_STR_EQ("FixtureUser", name);
		TEST_CHECK_STR_EQ("FixtureHost", domain);
		TEST_CHECK_EQ(28, sidBytes);
	}
	capacity = sizeof(sid);
	domainCapacity = sizeof(domain);
	TEST_CHECK(!LookupAccountNameA(NULL, "MissingFixtureAccount", sid, &capacity, domain, &domainCapacity, &use));
	TEST_CHECK_EQ(ERROR_NONE_MAPPED, GetLastError());
	return 0;
}
