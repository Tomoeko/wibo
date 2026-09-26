#pragma once

#include "ws2.h"

#include <atomic>
#include <memory>
#include <string>
#include <sys/socket.h>

namespace kernel32 {
struct CompletionBinding;
}

namespace ws2::detail {

struct Socket {
	int descriptor;
	int family;
	std::atomic<bool> connecting = false;
	std::atomic<bool> listening = false;
	bool overlapped = true;
	std::shared_ptr<const kernel32::CompletionBinding> completion;
	DWORD handleFlags = 1; // Protected by the socket registry mutex.
	explicit Socket(int descriptor, int family) : descriptor(descriptor), family(family) {}
	~Socket();
};
std::shared_ptr<Socket> findSocket(SOCKET handle);
void cleanupSockets();
bool getHandleInformation(SOCKET handle, DWORD *flags);
bool setHandleInformation(SOCKET handle, DWORD mask, DWORD flags);
int socketError(int error);
int failSocket(int error);
int addressToNative(LPCVOID address, int length, sockaddr_storage &result, socklen_t &resultLength);
int addressFromNative(const sockaddr *address, LPVOID result, int *length);

bool localHostName(std::string &name);
bool requireStarted();
void setLastError(int error);

} // namespace ws2::detail
