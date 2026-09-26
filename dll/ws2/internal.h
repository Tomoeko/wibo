#pragma once

#include "ws2.h"

#include <atomic>
#include <memory>
#include <string>
#include <sys/socket.h>

namespace ws2::detail {

struct Socket {
	int descriptor;
	int family;
	std::atomic<bool> connecting = false;
	std::atomic<bool> listening = false;
	explicit Socket(int descriptor, int family) : descriptor(descriptor), family(family) {}
	~Socket();
};
std::shared_ptr<Socket> findSocket(SOCKET handle);
void cleanupSockets();
int socketError(int error);
int failSocket(int error);
int addressToNative(LPCVOID address, int length, sockaddr_storage &result, socklen_t &resultLength);
int addressFromNative(const sockaddr *address, LPVOID result, int *length);

bool localHostName(std::string &name);
bool requireStarted();
void setLastError(int error);

} // namespace ws2::detail
