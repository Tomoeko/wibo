#include "test_assert.h"
#include <windows.h>

int main(void) {
	SECURITY_DESCRIPTOR descriptor;
	TEST_CHECK(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
	TEST_CHECK(SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED | SE_SACL_AUTO_INHERIT_REQ,
											SE_DACL_PROTECTED | SE_SACL_AUTO_INHERIT_REQ));
	TEST_CHECK_EQ(SE_DACL_PROTECTED | SE_SACL_AUTO_INHERIT_REQ, descriptor.Control);
	TEST_CHECK(SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_SACL_PROTECTED));
	TEST_CHECK_EQ(SE_SACL_AUTO_INHERIT_REQ, descriptor.Control);
	TEST_CHECK(!SetSecurityDescriptorControl(&descriptor, SE_DACL_PRESENT, SE_DACL_PRESENT));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	struct {
		SECURITY_DESCRIPTOR_RELATIVE relative;
		DWORD sentinel;
	} storage = {0};
	storage.relative.Revision = SECURITY_DESCRIPTOR_REVISION;
	storage.relative.Control = SE_SELF_RELATIVE;
	storage.sentinel = 0x12345678;
	TEST_CHECK(SetSecurityDescriptorControl(&storage.relative, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
	TEST_CHECK_EQ(SE_SELF_RELATIVE | SE_DACL_PROTECTED, storage.relative.Control);
	TEST_CHECK_EQ(0x12345678, storage.sentinel);
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	TEST_CHECK(GetSecurityDescriptorControl(&storage.relative, &control, &revision));
	TEST_CHECK_EQ(SE_SELF_RELATIVE | SE_DACL_PROTECTED, control);
	TEST_CHECK_EQ(SECURITY_DESCRIPTOR_REVISION, revision);
	storage.relative.Revision = 0;
	revision = 0x1234;
	control = 0x5678;
	TEST_CHECK(!GetSecurityDescriptorControl(&storage.relative, &control, &revision));
	TEST_CHECK_EQ(ERROR_UNKNOWN_REVISION, GetLastError());
	TEST_CHECK_EQ(0, revision);
	TEST_CHECK_EQ(0x5678, control);
	return 0;
}
