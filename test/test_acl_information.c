#include "test_assert.h"
#include <windows.h>

int main(void) {
	DWORD storage[16];
	PACL acl = (PACL)storage;
	TEST_CHECK(InitializeAcl(acl, sizeof(storage), ACL_REVISION));
	TEST_CHECK_EQ(sizeof(storage), acl->AclSize);
	TEST_CHECK_EQ(0, acl->Sbz2);
	ACL_SIZE_INFORMATION size;
	TEST_CHECK(GetAclInformation(acl, &size, sizeof(size), AclSizeInformation));
	TEST_CHECK_EQ(0, size.AceCount);
	TEST_CHECK_EQ(8, size.AclBytesInUse);
	TEST_CHECK_EQ(56, size.AclBytesFree);
	ACL_REVISION_INFORMATION revision;
	TEST_CHECK(GetAclInformation(acl, &revision, sizeof(revision), AclRevisionInformation));
	TEST_CHECK_EQ(ACL_REVISION, revision.AclRevision);
	const DWORD sid[] = {0x00000101, 0x05000000, 18};
	TEST_CHECK_EQ(12, GetLengthSid((PSID)sid));
	const DWORD longerSid[] = {0x00000501, 0x05000000, 21, 11, 22, 33, 1001};
	TEST_CHECK_EQ(28, GetLengthSid((PSID)longerSid));
	TEST_CHECK(AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, (PSID)sid));
	TEST_CHECK_EQ(sizeof(storage), acl->AclSize);
	TEST_CHECK(GetAclInformation(acl, &size, sizeof(size), AclSizeInformation));
	TEST_CHECK_EQ(1, size.AceCount);
	TEST_CHECK_EQ(28, size.AclBytesInUse);
	TEST_CHECK_EQ(36, size.AclBytesFree);
	void *ace = NULL;
	TEST_CHECK(GetAce(acl, 0, &ace));
	TEST_CHECK((BYTE *)ace == (BYTE *)acl + 8);
	TEST_CHECK_EQ(20, ((ACE_HEADER *)ace)->AceSize);
	DWORD copiedAce[5];
	memcpy(copiedAce, ace, sizeof(copiedAce));
	copiedAce[1] = GENERIC_WRITE;
	TEST_CHECK(AddAce(acl, ACL_REVISION, 0, copiedAce, sizeof(copiedAce)));
	TEST_CHECK(GetAce(acl, 0, &ace));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(GENERIC_WRITE, ((ACCESS_ALLOWED_ACE *)ace)->Mask);
	TEST_CHECK(GetAce(acl, 1, &ace));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(GENERIC_READ, ((ACCESS_ALLOWED_ACE *)ace)->Mask);
	TEST_CHECK(GetAclInformation(acl, &size, sizeof(size), AclSizeInformation));
	TEST_CHECK_EQ(2, size.AceCount);
	TEST_CHECK_EQ(48, size.AclBytesInUse);
	TEST_CHECK_EQ(16, size.AclBytesFree);
	TEST_CHECK(!AddAce(acl, ACL_REVISION, MAXDWORD, copiedAce, sizeof(copiedAce)));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(2, acl->AceCount);
	TEST_CHECK(!GetAce(acl, 2, &ace));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(!GetAclInformation(acl, &size, sizeof(size) - 1, AclSizeInformation));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(!GetAclInformation(acl, &size, sizeof(size), (ACL_INFORMATION_CLASS)99));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(InitializeAcl(acl, 63, ACL_REVISION));
	TEST_CHECK_EQ(63, acl->AclSize);
	return 0;
}
