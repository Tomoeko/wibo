#include "test_assert.h"
#include <windows.h>

int main(void) {
	char directory[MAX_PATH], path[MAX_PATH];
	TEST_CHECK(GetTempPathA(MAX_PATH, directory) != 0);
	TEST_CHECK(GetTempFileNameA(directory, "sdf", 0, path) != 0);
	DWORD needed = 0;
	TEST_CHECK(!GetFileSecurityA(path, DACL_SECURITY_INFORMATION, NULL, 0, &needed));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK(needed >= sizeof(SECURITY_DESCRIPTOR_RELATIVE));
	BYTE *buffer = HeapAlloc(GetProcessHeap(), 0, needed);
	TEST_CHECK(buffer != NULL);
	memset(buffer, 0x55, needed);
	DWORD repeated = 0;
	TEST_CHECK(!GetFileSecurityA(path, DACL_SECURITY_INFORMATION, buffer, needed - 1, &repeated));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(needed, repeated);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		for (DWORD i = 0; i < needed; ++i)
			TEST_CHECK_EQ(0x55, buffer[i]);
	}
	TEST_CHECK(GetFileSecurityA(path, DACL_SECURITY_INFORMATION, buffer, needed, &repeated));
	SECURITY_DESCRIPTOR_RELATIVE *relative = (SECURITY_DESCRIPTOR_RELATIVE *)buffer;
	TEST_CHECK(relative->Control & SE_SELF_RELATIVE);
	BOOL present = FALSE, defaulted = TRUE;
	PACL acl = NULL;
	TEST_CHECK(GetSecurityDescriptorDacl(buffer, &present, &acl, &defaulted));
	TEST_CHECK(present);
	TEST_CHECK(acl != NULL);
	TEST_CHECK((BYTE *)acl == buffer + relative->Dacl);
	TEST_CHECK((BYTE *)acl >= buffer + sizeof(*relative));
	TEST_CHECK((BYTE *)acl + acl->AclSize <= buffer + needed);
	SECURITY_DESCRIPTOR absolute;
	TEST_CHECK(InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION));
	TEST_CHECK(SetSecurityDescriptorDacl(&absolute, TRUE, acl, FALSE));
	TEST_CHECK(SetFileSecurityA(path, DACL_SECURITY_INFORMATION, &absolute));
	TEST_CHECK(SetFileSecurityA(path, DACL_SECURITY_INFORMATION, buffer));
	WCHAR widePath[MAX_PATH];
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, MAX_PATH));
	TEST_CHECK(GetFileSecurityW(widePath, DACL_SECURITY_INFORMATION, buffer, needed, &repeated));
	TEST_CHECK(SetFileSecurityW(widePath, DACL_SECURITY_INFORMATION, buffer));
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		absolute.Owner = (PSID)(uintptr_t)1;
		absolute.Group = (PSID)(uintptr_t)1;
		absolute.Sacl = (PACL)(uintptr_t)1;
		TEST_CHECK(SetFileSecurityW(widePath, DACL_SECURITY_INFORMATION, &absolute));
		TEST_CHECK(!SetFileSecurityW(widePath, LABEL_SECURITY_INFORMATION, &absolute));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	}
	TEST_CHECK(!SetSecurityDescriptorDacl(buffer, TRUE, NULL, FALSE));
	TEST_CHECK_EQ(ERROR_INVALID_SECURITY_DESCR, GetLastError());
	HeapFree(GetProcessHeap(), 0, buffer);
	TEST_CHECK(DeleteFileA(path));
	SECURITY_DESCRIPTOR_RELATIVE empty;
	memset(&empty, 0, sizeof(empty));
	empty.Revision = SECURITY_DESCRIPTOR_REVISION;
	empty.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
	acl = (PACL)(uintptr_t)0x1234;
	TEST_CHECK(GetSecurityDescriptorDacl(&empty, &present, &acl, &defaulted));
	TEST_CHECK(present && acl == NULL && !defaulted);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		empty.Control = SE_SELF_RELATIVE;
		acl = (PACL)(uintptr_t)0x1234;
		defaulted = 77;
		TEST_CHECK(GetSecurityDescriptorDacl(&empty, &present, &acl, &defaulted));
		TEST_CHECK(!present && acl == (PACL)(uintptr_t)0x1234 && defaulted == 77);
		BYTE invalid[128];
		memset(invalid, 0x55, sizeof(invalid));
		TEST_CHECK(!GetFileSecurityA("bad-offset", DACL_SECURITY_INFORMATION, invalid, sizeof(invalid), &needed));
		TEST_CHECK_EQ(ERROR_INVALID_SECURITY_DESCR, GetLastError());
		TEST_CHECK(!GetFileSecurityA("bad-ace", DACL_SECURITY_INFORMATION, invalid, sizeof(invalid), &needed));
		TEST_CHECK_EQ(ERROR_INVALID_SECURITY_DESCR, GetLastError());
		for (size_t i = 0; i < sizeof(invalid); ++i)
			TEST_CHECK_EQ(0x55, invalid[i]);
	}
	return 0;
}
