#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include "test_assert.h"

typedef struct AcceptState {
	SOCKET listener;
	HANDLE entered;
	volatile LONG callbacks;
	SOCKET accepted;
	int acceptedError;
	BOOL cancelInCallback;
} AcceptState;

static void CALLBACK on_apc(ULONG_PTR argument) {
	AcceptState *state = (AcceptState *)argument;
	if (state->cancelInCallback)
		TEST_CHECK(CancelIo((HANDLE)state->listener));
	InterlockedIncrement(&state->callbacks);
}

static DWORD WINAPI accept_thread(LPVOID argument) {
	AcceptState *state = (AcceptState *)argument;
	SetEvent(state->entered);
	state->accepted = accept(state->listener, NULL, NULL);
	state->acceptedError = state->accepted == INVALID_SOCKET ? WSAGetLastError() : 0;
	return 0;
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	AcceptState state = {0};
	state.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	state.accepted = INVALID_SOCKET;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(state.listener != INVALID_SOCKET && state.entered != NULL);
	SOCKADDR_IN address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_CHECK_EQ(0, bind(state.listener, (SOCKADDR *)&address, sizeof(address)));
	TEST_CHECK_EQ(0, listen(state.listener, 4));
	int addressLength = sizeof(address);
	TEST_CHECK_EQ(0, getsockname(state.listener, (SOCKADDR *)&address, &addressLength));
	HANDLE thread = CreateThread(NULL, 0, accept_thread, &state, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 2000));
	Sleep(100);
	TEST_CHECK_EQ(1, QueueUserAPC(on_apc, thread, (ULONG_PTR)&state));
	for (unsigned attempt = 0; attempt < 200 && !state.callbacks; ++attempt)
		Sleep(10);
	const LONG beforeConnection = state.callbacks;
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(client != INVALID_SOCKET);
	TEST_CHECK_EQ(0, connect(client, (SOCKADDR *)&address, addressLength));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(state.accepted != INVALID_SOCKET);
	TEST_CHECK_EQ(0, closesocket(state.accepted));
	TEST_CHECK_EQ(0, closesocket(client));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(ResetEvent(state.entered));
	state.accepted = INVALID_SOCKET;
	state.acceptedError = 0;
	state.callbacks = 0;
	state.cancelInCallback = TRUE;
	thread = CreateThread(NULL, 0, accept_thread, &state, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 2000));
	Sleep(100);
	TEST_CHECK_EQ(1, QueueUserAPC(on_apc, thread, (ULONG_PTR)&state));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK_EQ(1, state.callbacks);
	TEST_CHECK_EQ(INVALID_SOCKET, state.accepted);
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, state.acceptedError);
	state.cancelInCallback = FALSE;
	u_long nonblocking = 1;
	TEST_CHECK_EQ(0, ioctlsocket(state.listener, FIONBIO, &nonblocking));
	state.callbacks = 0;
	TEST_CHECK_EQ(1, QueueUserAPC(on_apc, GetCurrentThread(), (ULONG_PTR)&state));
	TEST_CHECK_EQ(INVALID_SOCKET, accept(state.listener, NULL, NULL));
	TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
	TEST_CHECK_EQ(0, state.callbacks);
	TEST_CHECK_EQ(WAIT_IO_COMPLETION, SleepEx(0, TRUE));
	TEST_CHECK_EQ(1, state.callbacks);
	TEST_CHECK_EQ(0, closesocket(state.listener));
	CloseHandle(thread);
	CloseHandle(state.entered);
	TEST_CHECK_EQ(0, WSACleanup());
	TEST_CHECK_EQ(1, beforeConnection);
	TEST_CHECK_EQ(1, state.callbacks);
	return 0;
}
