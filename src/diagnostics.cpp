#include "diagnostics.h"

#include "common.h"
#include "processes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct DiagnosticDescriptor {
	int value = -1;
	~DiagnosticDescriptor() {
		if (value >= 0)
			close(value);
	}
};

DiagnosticDescriptor gDiagnosticDescriptor;

struct DiagnosticRelay {
	int sender = -1;
	int receiver = -1;
	int destination = -1;
	std::atomic_uint64_t dropped{0};
};

DiagnosticRelay gDiagnosticRelay;
std::atomic_flag gDebugFormatting = ATOMIC_FLAG_INIT;
std::atomic_uint64_t gFormattingDropped{0};

void writeDescriptor(int descriptor, const char *message, size_t length) {
	while (length) {
		const ssize_t written = write(descriptor, message, length);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			break;
		message += written;
		length -= static_cast<size_t>(written);
	}
}

void *relayDiagnostics(void *) {
	// recv initializes each emitted span.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
	std::array<char, 8192> message;
	auto lastNotice = std::chrono::steady_clock::time_point{};
	for (;;) {
		const ssize_t received = recv(gDiagnosticRelay.receiver, message.data(), message.size(), 0);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			break;
		const auto now = std::chrono::steady_clock::now();
		if (gDiagnosticRelay.dropped.load(std::memory_order_relaxed) &&
			now - lastNotice >= std::chrono::milliseconds(100)) {
			const uint64_t dropped = gDiagnosticRelay.dropped.exchange(0, std::memory_order_relaxed);
			lastNotice = now;
			char notice[128];
			const int count = std::snprintf(notice, sizeof(notice),
											"wibo: %llu debug messages omitted while diagnostic output was busy\n",
											static_cast<unsigned long long>(dropped));
			if (count > 0 && static_cast<size_t>(count) < sizeof(notice))
				writeDescriptor(gDiagnosticRelay.destination, notice, static_cast<size_t>(count));
		}
		writeDescriptor(gDiagnosticRelay.destination, message.data(), static_cast<size_t>(received));
	}
	close(gDiagnosticRelay.receiver);
	close(gDiagnosticRelay.destination);
	return nullptr;
}

int independentDescriptor(int descriptor) {
	if (descriptor < 0)
		return -1;
	if (descriptor < 3) {
		const int independent = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
		const int savedError = errno;
		close(descriptor);
		errno = savedError;
		return independent;
	}
	if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
		const int savedError = errno;
		close(descriptor);
		errno = savedError;
		return -1;
	}
	return descriptor;
}

bool initializeRelay() {
	if (gDiagnosticRelay.sender >= 0)
		return true;
	int descriptors[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) != 0)
		return false;
	DiagnosticDescriptor sender{independentDescriptor(descriptors[0])};
	DiagnosticDescriptor receiver{independentDescriptor(descriptors[1])};
	DiagnosticDescriptor destination{fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)};
	// Endpoint status also covers a full send buffer; per-call flags prevent
	// waiting for its lock. Both affect only this private transport.
	const int senderFlags = sender.value >= 0 ? fcntl(sender.value, F_GETFL) : -1;
	constexpr int bufferBytes = 64 * 1024;
	if (sender.value < 0 || receiver.value < 0 || destination.value < 0 || senderFlags < 0 ||
		fcntl(sender.value, F_SETFL, senderFlags | O_NONBLOCK) != 0 ||
		setsockopt(sender.value, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes)) != 0 ||
		setsockopt(receiver.value, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes)) != 0)
		return false;
	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes) != 0)
		return false;
	const int detached = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	gDiagnosticRelay.receiver = receiver.value;
	gDiagnosticRelay.destination = destination.value;
	const int created = detached ? detached : pthread_create(&thread, &attributes, relayDiagnostics, nullptr);
	pthread_attr_destroy(&attributes);
	if (created) {
		gDiagnosticRelay.receiver = gDiagnosticRelay.destination = -1;
		return false;
	}
	// The raw host relay has no guest TEB or thread handle. It alone may wait
	// for stderr; a suspended guest never owns that blocking write operation.
	gDiagnosticRelay.sender = sender.value;
	sender.value = receiver.value = destination.value = -1;
	return true;
}

void writeMessage(bool debug, const char *message, size_t length) {
	if (!debug || gDiagnosticDescriptor.value >= 0) {
		writeDescriptor(gDiagnosticDescriptor.value >= 0 ? gDiagnosticDescriptor.value : STDERR_FILENO, message,
						length);
		return;
	}
	if (gDiagnosticRelay.sender < 0)
		return;
	ssize_t sent;
	do {
		sent = send(gDiagnosticRelay.sender, message, length, MSG_DONTWAIT);
	} while (sent < 0 && errno == EINTR);
	// Datagram delivery is best effort under backpressure; never delay a guest
	// or wait for the relay at process exit. Fatal diagnostics remain synchronous.
	if (sent < 0 || static_cast<size_t>(sent) != length)
		gDiagnosticRelay.dropped.fetch_add(1, std::memory_order_relaxed);
}

void logMessage(bool debug, const char *format, va_list arguments) {
	const int savedError = errno;
	// Host printf may use shared internal state even with a private buffer.
	// Never wait for a formatter owned by a suspended guest thread.
	if (debug && gDebugFormatting.test_and_set(std::memory_order_acquire)) {
		gFormattingDropped.fetch_add(1, std::memory_order_relaxed);
		errno = savedError;
		return;
	}
	// A suspended guest thread can be inside any host shim. Keep diagnostics
	// independent of shared stdio sink locks and explicit heap allocation.
	std::array<char, 8192> message; // NOLINT(cppcoreguidelines-pro-type-member-init): only the formatted span is read.
	size_t used = 0;
	if (debug) {
		used = std::min<size_t>(wibo::debugIndent, message.size() / 2);
		std::fill_n(message.data(), used, '\t');
		const int prefix =
			std::snprintf(message.data() + used, message.size() - used, "[thread %x] ", wibo::getThreadId());
		if (prefix > 0)
			used += std::min<size_t>(static_cast<size_t>(prefix), message.size() - used - 1);
		const uint64_t dropped = gFormattingDropped.exchange(0, std::memory_order_relaxed);
		if (dropped) {
			const int notice =
				std::snprintf(message.data() + used, message.size() - used, "[debug messages omitted: %llu] ",
							  static_cast<unsigned long long>(dropped));
			if (notice > 0)
				used += std::min<size_t>(static_cast<size_t>(notice), message.size() - used - 1);
		}
	}
	const size_t available = message.size() - used;
	const int count = std::vsnprintf(message.data() + used, available, format, arguments);
	if (debug)
		gDebugFormatting.clear(std::memory_order_release);
	if (count >= 0) {
		used += std::min<size_t>(static_cast<size_t>(count), available - 1);
		if (static_cast<size_t>(count) >= available) {
			constexpr char marker[] = " [diagnostic truncated]\n";
			constexpr size_t markerLength = sizeof(marker) - 1;
			std::memcpy(message.data() + used - markerLength, marker, markerLength);
		}
		writeMessage(debug, message.data(), used);
	}
	errno = savedError;
}

} // namespace

void wibo::initializeDiagnostics() {
	const int savedError = errno;
	const char *path = std::getenv("WIBO_DIAGNOSTIC_LOG");
	if (path && *path) {
		const int descriptor = independentDescriptor(open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
		if (descriptor < 0) {
			const int error = errno;
			wibo::diagnosticLog("wibo: cannot open diagnostic log '%s': %s; using stderr\n", path,
								std::strerror(error));
		} else {
			if (gDiagnosticDescriptor.value >= 0)
				close(gDiagnosticDescriptor.value);
			gDiagnosticDescriptor.value = descriptor;
		}
	}
	if (wibo::debugEnabled && gDiagnosticDescriptor.value < 0 && !initializeRelay())
		wibo::diagnosticLog("wibo: cannot initialize diagnostic relay; debug output unavailable\n");
	errno = savedError;
}

void wibo::diagnosticLog(const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	logMessage(false, format, arguments);
	va_end(arguments);
}

void wibo::debug_log(const char *fmt, ...) {
	if (!wibo::debugEnabled)
		return;
	va_list arguments;
	va_start(arguments, fmt);
	logMessage(true, fmt, arguments);
	va_end(arguments);
}
