#include <mswsock.h>
#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

static void *extension(SOCKET socket, GUID id) {
	void *result = NULL;
	DWORD bytes = 0;
	TEST_CHECK_EQ(0, WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &result, sizeof(result),
							  &bytes, NULL, NULL));
	TEST_CHECK(result != NULL);
	TEST_CHECK_EQ(sizeof(result), bytes);
	return result;
}

static LPFN_ACCEPTEX threadAccept;
static SOCKET threadListener, threadAccepted;
static OVERLAPPED threadOperation;
static BYTE threadBuffer[80];
static DWORD WINAPI issueAndExit(void *unused) {
	(void)unused;
	DWORD received = 0;
	TEST_CHECK(!threadAccept(threadListener, threadAccepted, threadBuffer, 0, 40, 40, &received, &threadOperation));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	return 0;
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(listener != INVALID_SOCKET);
	LPFN_ACCEPTEX acceptEx = (LPFN_ACCEPTEX)extension(listener, (GUID)WSAID_ACCEPTEX);
	LPFN_GETACCEPTEXSOCKADDRS addresses =
		(LPFN_GETACCEPTEXSOCKADDRS)extension(listener, (GUID)WSAID_GETACCEPTEXSOCKADDRS);
	HMODULE legacy = LoadLibraryA("wsock32.dll");
	TEST_CHECK(legacy != NULL);
	LPFN_ACCEPTEX legacyAccept = (LPFN_ACCEPTEX)(void *)GetProcAddress(legacy, MAKEINTRESOURCEA(1141));
	TEST_CHECK(legacyAccept != NULL);
	LPFN_GETACCEPTEXSOCKADDRS legacyAddresses =
		(LPFN_GETACCEPTEXSOCKADDRS)(void *)GetProcAddress(legacy, MAKEINTRESOURCEA(1142));
	TEST_CHECK(legacyAddresses != NULL);
	typedef ULONG(WSAAPI * ParseAddress)(const char *);
	typedef char *(WSAAPI * FormatAddress)(IN_ADDR);
	typedef int(WSAAPI * ControlSocket)(SOCKET, long, ULONG *);
	ParseAddress parse = (ParseAddress)(void *)GetProcAddress(legacy, MAKEINTRESOURCEA(10));
	FormatAddress format = (FormatAddress)(void *)GetProcAddress(legacy, MAKEINTRESOURCEA(11));
	ControlSocket control = (ControlSocket)(void *)GetProcAddress(legacy, MAKEINTRESOURCEA(12));
	TEST_CHECK(parse && format && control);
	IN_ADDR numeric;
	numeric.S_un.S_addr = parse("127.0.0.1");
	TEST_CHECK_EQ(0x0100007f, numeric.S_un.S_addr);
	TEST_CHECK_STR_EQ("127.0.0.1", format(numeric));
	ULONG mode = 0;
	TEST_CHECK_EQ(0, control(listener, FIONBIO, &mode));
	acceptEx = legacyAccept;
	addresses = legacyAddresses;
	GUID id = WSAID_ACCEPTEX;
	void *unchanged = (void *)(ULONG_PTR)0x71;
	DWORD returned = 0x51;
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK_EQ(SOCKET_ERROR, WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &unchanged,
											 sizeof(unchanged) - 1, &returned, NULL, NULL));
		TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
		TEST_CHECK(unchanged == (void *)(ULONG_PTR)0x71);
		TEST_CHECK_EQ(0x51, returned);
	}
	id.Data1 = 0;
	TEST_CHECK_EQ(SOCKET_ERROR, WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &unchanged,
										 sizeof(unchanged), &returned, NULL, NULL));
	TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	int noDelay = 1;
	TEST_CHECK_EQ(0, setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay)));
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_CHECK_EQ(0, bind(listener, (const struct sockaddr *)&address, sizeof(address)));
	TEST_CHECK_EQ(0, listen(listener, 8));
	int length = sizeof(address);
	TEST_CHECK_EQ(0, getsockname(listener, (struct sockaddr *)&address, &length));
	const ULONG_PTR key = ((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1)) | 0x71;
	HANDLE port = CreateIoCompletionPort((HANDLE)listener, NULL, key, 1);
	TEST_CHECK(port != NULL);
	for (unsigned initial = 0; initial < 2; ++initial) {
		SOCKET accepted = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(accepted != INVALID_SOCKET);
		BYTE buffer[101];
		memset(buffer, 0x71, sizeof(buffer));
		OVERLAPPED operation = {0};
		DWORD received = 0x51;
		DWORD dataLength = initial ? 16 : 0;
		TEST_CHECK(!acceptEx(listener, accepted, buffer + 1, dataLength, 40, 40, &received, &operation));
		TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
		if (getenv("WIBO_FIXTURE_RUNTIME"))
			TEST_CHECK_EQ(0x51, received);
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(client != INVALID_SOCKET);
		TEST_CHECK_EQ(0, connect(client, (const struct sockaddr *)&address, sizeof(address)));
		if (initial)
			TEST_CHECK_EQ(5, send(client, "hello", 5, 0));
		ULONG_PTR receivedKey = 0;
		OVERLAPPED *completed = NULL;
		TEST_CHECK(GetQueuedCompletionStatus(port, &received, &receivedKey, &completed, 5000));
		TEST_CHECK_U64_EQ(key, receivedKey);
		TEST_CHECK(completed == &operation);
		TEST_CHECK_EQ(initial ? 5 : 0, received);
		TEST_CHECK_EQ(0, operation.Internal);
		TEST_CHECK_EQ(received, operation.InternalHigh);
		if (initial)
			TEST_CHECK(memcmp(buffer + 1, "hello", 5) == 0);
		TEST_CHECK_EQ(0x71, buffer[0]);
		TEST_CHECK_EQ(0x71, buffer[1 + dataLength + 80]);
		struct sockaddr *local = NULL, *remote = NULL;
		int localLength = 0, remoteLength = 0;
		addresses(buffer + 1, dataLength, 40, 40, &local, &localLength, &remote, &remoteLength);
		TEST_CHECK_EQ(sizeof(struct sockaddr_in), localLength);
		TEST_CHECK_EQ(sizeof(struct sockaddr_in), remoteLength);
		TEST_CHECK(local && remote);
		TEST_CHECK_EQ(AF_INET, local->sa_family);
		TEST_CHECK_EQ(AF_INET, remote->sa_family);
		TEST_CHECK(memcmp(local, &address, sizeof(address)) == 0);
		TEST_CHECK_EQ(
			0, setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&listener, sizeof(listener)));
		noDelay = 0;
		int optionLength = sizeof(noDelay);
		TEST_CHECK_EQ(0, getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, (char *)&noDelay, &optionLength));
		TEST_CHECK(noDelay != 0);
		TEST_CHECK_EQ(0, closesocket(client));
		TEST_CHECK_EQ(0, closesocket(accepted));
	}
	SOCKET eventAccepted = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	BYTE eventBuffer[80];
	OVERLAPPED eventOperation = {0};
	HANDLE event = CreateEventW(NULL, TRUE, TRUE, NULL);
	TEST_CHECK(event != NULL);
	eventOperation.hEvent = (HANDLE)((ULONG_PTR)event | 1);
	DWORD eventBytes = 0;
	TEST_CHECK(!acceptEx(listener, eventAccepted, eventBuffer, 0, 40, 40, &eventBytes, &eventOperation));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(event, 0));
	SOCKET eventClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK_EQ(0, connect(eventClient, (const struct sockaddr *)&address, sizeof(address)));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 5000));
	TEST_CHECK(GetOverlappedResult((HANDLE)listener, &eventOperation, &eventBytes, FALSE));
	TEST_CHECK_EQ(0, eventBytes);
	ULONG_PTR eventKey = 0;
	OVERLAPPED *eventCompleted = NULL;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &eventBytes, &eventKey, &eventCompleted, 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, GetLastError());
	TEST_CHECK_EQ(0, closesocket(eventClient));
	TEST_CHECK_EQ(0, closesocket(eventAccepted));
	TEST_CHECK(CloseHandle(event));
	threadAccept = acceptEx;
	threadListener = listener;
	threadAccepted = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	HANDLE issuingThread = CreateThread(NULL, 0, issueAndExit, NULL, 0, NULL);
	TEST_CHECK(issuingThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(issuingThread, 5000));
	TEST_CHECK(!GetQueuedCompletionStatus(port, &eventBytes, &eventKey, &eventCompleted, 200));
	if (!getenv("WIBO_FIXTURE_RUNTIME") && GetLastError() == WAIT_TIMEOUT) {
		TEST_CHECK_EQ(0, closesocket(threadAccepted));
		threadAccepted = INVALID_SOCKET;
		TEST_CHECK(!GetQueuedCompletionStatus(port, &eventBytes, &eventKey, &eventCompleted, 5000));
	}
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK_U64_EQ(key, eventKey);
	TEST_CHECK(eventCompleted == &threadOperation);
	if (threadAccepted != INVALID_SOCKET)
		TEST_CHECK_EQ(0, closesocket(threadAccepted));
	TEST_CHECK(CloseHandle(issuingThread));
	SOCKET cancelled = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	BYTE buffer[80];
	OVERLAPPED operation = {0};
	DWORD received = 0x51;
	TEST_CHECK(!acceptEx(listener, cancelled, buffer, 0, 40, 40, &received, &operation));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	TEST_CHECK_EQ(0, closesocket(cancelled));
	ULONG_PTR receivedKey = 0;
	OVERLAPPED *completed = NULL;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &received, &receivedKey, &completed, 5000));
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK_U64_EQ(key, receivedKey);
	TEST_CHECK(completed == &operation);
	TEST_CHECK_EQ(0, received);
	SOCKET pending[32];
	OVERLAPPED pendingOperations[32] = {0};
	BYTE pendingBuffers[32][80];
	BOOL seen[32] = {0};
	for (unsigned i = 0; i < 32; ++i) {
		pending[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(pending[i] != INVALID_SOCKET);
		TEST_CHECK(!acceptEx(listener, pending[i], pendingBuffers[i], 0, 40, 40, &received, &pendingOperations[i]));
		TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	}
	TEST_CHECK_EQ(0, closesocket(listener));
	for (unsigned i = 0; i < 32; ++i) {
		TEST_CHECK(!GetQueuedCompletionStatus(port, &received, &receivedKey, &completed, 5000));
		TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
		TEST_CHECK_U64_EQ(key, receivedKey);
		TEST_CHECK(completed >= pendingOperations && completed < pendingOperations + 32);
		const unsigned index = (unsigned)(completed - pendingOperations);
		TEST_CHECK(!seen[index]);
		seen[index] = TRUE;
		TEST_CHECK_EQ(0, received);
	}
	for (unsigned i = 0; i < 32; ++i)
		TEST_CHECK_EQ(0, closesocket(pending[i]));
	TEST_CHECK(CloseHandle(port));
	TEST_CHECK(FreeLibrary(legacy));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
