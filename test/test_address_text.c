#include "test_assert.h"
#include <winsock2.h>

static DWORD WINAPI worker(void *context) {
	char *parent = (char *)context;
	IN_ADDR address;
	address.S_un.S_addr = 0x04030201;
	char *text = inet_ntoa(address);
	TEST_CHECK(text != NULL);
	TEST_CHECK_STR_EQ("1.2.3.4", text);
	TEST_CHECK_STR_EQ("255.255.255.255", parent);
	return 0;
}
int main(void) {
	struct {
		const char *text;
		ULONG address;
	} cases[] = {{"1.2.3.4", 0x04030201},	  {"127.1", 0x0100007F},
				 {"0177.1", 0x0100007F},	  {"0x7f.1", 0x0100007F},
				 {"1.2.3", 0x03000201},		  {"0x7f000001", 0x0100007F},
				 {"1.16777215", 0xFFFFFF01},  {"1.2.65535", 0xFFFF0201},
				 {"1 ", 0x01000000},		  {" ", 0},
				 {"", INADDR_NONE},			  {"08", INADDR_NONE},
				 {"0x", INADDR_NONE},		  {"1.16777216", INADDR_NONE},
				 {"1.2.65536", INADDR_NONE},  {"256.0.0.1", INADDR_NONE},
				 {"4294967296", INADDR_NONE}, {"1.2.3.", INADDR_NONE},
				 {"1..2", INADDR_NONE}};
	for (unsigned index = 0; index != sizeof(cases) / sizeof(cases[0]); ++index) {
		WSASetLastError(0x1234);
		TEST_CHECK_EQ(cases[index].address, inet_addr(cases[index].text));
		TEST_CHECK_EQ(0x1234, WSAGetLastError());
	}
	TEST_CHECK_EQ(INADDR_NONE, inet_addr(NULL));
	TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
	IN_ADDR address;
	address.S_un.S_addr = 0;
	char *text = inet_ntoa(address);
	TEST_CHECK(text != NULL);
	TEST_CHECK_STR_EQ("0.0.0.0", text);
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	address.S_un.S_addr = 0x78563412;
	text = inet_ntoa(address);
	TEST_CHECK(text != NULL);
	TEST_CHECK_STR_EQ("18.52.86.120", text);
	address.S_un.S_addr = 0xFFFFFFFF;
	text = inet_ntoa(address);
	TEST_CHECK(text != NULL);
	TEST_CHECK_STR_EQ("255.255.255.255", text);
	HANDLE thread = CreateThread(NULL, 0, worker, text, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD status = 1;
	TEST_CHECK(GetExitCodeThread(thread, &status));
	TEST_CHECK_EQ(0, status);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK_STR_EQ("255.255.255.255", text);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		HMODULE module = GetModuleHandleA("ws2_32.dll");
		char *(WSAAPI * byOrdinal)(IN_ADDR) = (void *)GetProcAddress(module, (LPCSTR)12);
		TEST_CHECK(byOrdinal != NULL);
		address.S_un.S_addr = 0x0100007F;
		TEST_CHECK_STR_EQ("127.0.0.1", byOrdinal(address));
		ULONG(WSAAPI * parseByOrdinal)(const char *) = (void *)GetProcAddress(module, (LPCSTR)11);
		TEST_CHECK(parseByOrdinal != NULL);
		TEST_CHECK_EQ(0x0100007F, parseByOrdinal("127.0.0.1"));
	}
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
