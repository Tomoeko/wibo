#include "ws2.h"

#include "common.h"
#include "context.h"
#include "heap.h"
#include "ws2/internal.h"

#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kWinInet6 = 23;
constexpr int kPassive = 1;
constexpr int kCanonName = 2;
constexpr int kNumericHost = 4;
constexpr int kNumericService = 8;
constexpr int kAll = 0x100;
constexpr int kAddrConfig = 0x400;
constexpr int kV4Mapped = 0x800;
constexpr int kSupportedFlags = kPassive | kCanonName | kNumericHost | kNumericService | kAll | kAddrConfig | kV4Mapped;

struct AddressRegistry {
	std::mutex mutex;
	std::unordered_map<ADDRINFOA *, ADDRINFOA *> nodes;
	~AddressRegistry() {
		for (const auto &[address, next] : nodes)
			wibo::heap::guestFree(address);
	}
};

AddressRegistry &addressRegistry() {
	static AddressRegistry registry;
	return registry;
}

int resolutionError(int error) {
	switch (error) {
	case EAI_AGAIN:
		return 11002;
	case EAI_BADFLAGS:
		return 10022;
	case EAI_FAIL:
		return 11003;
	case EAI_FAMILY:
		return 10047;
	case EAI_MEMORY:
		return 8;
	case EAI_NONAME:
		return 11001;
	case EAI_SERVICE:
		return 10109;
	case EAI_SOCKTYPE:
		return 10044;
	default:
		return 11003;
	}
}

int nativeFlags(int flags) {
	int result = 0;
	if (flags & kPassive)
		result |= AI_PASSIVE;
	if (flags & kCanonName)
		result |= AI_CANONNAME;
	if (flags & kNumericHost)
		result |= AI_NUMERICHOST;
	if (flags & kNumericService)
		result |= AI_NUMERICSERV;
	if (flags & kAll)
		result |= AI_ALL;
	if (flags & kAddrConfig)
		result |= AI_ADDRCONFIG;
	if (flags & kV4Mapped)
		result |= AI_V4MAPPED;
	return result;
}

int windowsFlags(int flags) {
	int result = 0;
	if (flags & AI_PASSIVE)
		result |= kPassive;
	if (flags & AI_CANONNAME)
		result |= kCanonName;
	if (flags & AI_NUMERICHOST)
		result |= kNumericHost;
	if (flags & AI_NUMERICSERV)
		result |= kNumericService;
	if (flags & AI_ALL)
		result |= kAll;
	if (flags & AI_ADDRCONFIG)
		result |= kAddrConfig;
	if (flags & AI_V4MAPPED)
		result |= kV4Mapped;
	return result;
}

bool asciiName(LPCSTR name) {
	if (name)
		for (const auto *byte = reinterpret_cast<const unsigned char *>(name); *byte; ++byte)
			if (*byte >= 128)
				return false;
	return true;
}

bool parseIpv4(const char *text, ULONG &result) {
	constexpr ULONG invalid = 0xffffffff;
	if (!*text)
		return false;
	auto whitespace = [](char value) { return value == ' ' || (value >= '\t' && value <= '\r'); };
	if (whitespace(*text)) {
		while (whitespace(*text))
			++text;
		if (*text)
			return false;
		result = 0;
		return true;
	}
	std::array<uint32_t, 4> parts{};
	size_t count = 0;
	while (true) {
		unsigned base = 10;
		bool digitSeen = false;
		if (*text == '0') {
			++text;
			base = 8;
			digitSeen = true;
			if (*text == 'x' || *text == 'X') {
				++text;
				base = 16;
				digitSeen = false;
			}
		}
		uint32_t value = 0;
		while (*text) {
			const unsigned digit = *text >= '0' && *text <= '9'	  ? *text - '0'
								   : *text >= 'a' && *text <= 'f' ? *text - 'a' + 10
								   : *text >= 'A' && *text <= 'F' ? *text - 'A' + 10
																  : 16;
			if (digit >= base)
				break;
			if (value > (invalid - digit) / base)
				return false;
			value = value * base + digit;
			digitSeen = true;
			++text;
		}
		if (!digitSeen)
			return false;
		parts[count++] = value;
		if (*text != '.')
			break;
		if (count == parts.size())
			return false;
		++text;
	}
	if (*text && !whitespace(*text))
		return false;
	uint32_t address = parts[count - 1];
	if (static_cast<uint64_t>(address) >= (uint64_t{1} << ((5 - count) * 8)))
		return false;
	for (size_t index = 0; index + 1 < count; ++index) {
		if (parts[index] > 255)
			return false;
		address |= parts[index] << (24 - index * 8);
	}
	result = __builtin_bswap32(address);
	return true;
}

bool decimal(std::string_view text, uint32_t &value) {
	if (text.empty())
		return false;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

namespace ws2 {
ULONG WINAPI inet_addr(LPCSTR text) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("inet_addr(%s)\n", text ? text : "(null)");
	if (!text) {
		detail::setLastError(10014);
		return 0xffffffff;
	}
	ULONG result = 0;
	return parseIpv4(text, result) ? result : 0xffffffff;
}

int WINAPI WSAStringToAddressW(LPWSTR input, int family, LPCVOID protocol, LPVOID output, int *length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAStringToAddressW(%p, %d, %p, %p, %p)\n", input, family, protocol, output, length);
	if (!detail::requireStarted())
		return -1;
	if (!input || !output || !length)
		return detail::failSocket(10014);
	if (family != AF_INET && family != kWinInet6)
		return detail::failSocket(10022);
	if (protocol)
		return detail::failSocket(10045);
	const int size = family == AF_INET ? 16 : 28;
	if (*length < size) {
		*length = size;
		return detail::failSocket(10014);
	}
	std::string text;
	for (size_t index = 0; input[index]; ++index) {
		if (index >= 128 || input[index] > 127)
			return detail::failSocket(10022);
		text.push_back(static_cast<char>(input[index]));
	}
	// Parsing failures leave a cleared address record and retain the supplied capacity.
	std::memset(output, 0, size);
	if (text.empty())
		return detail::failSocket(10022);
	std::string_view address = text;
	std::string_view portText;
	bool hasPort = false;
	if (family == AF_INET) {
		const auto colon = address.find(':');
		if (colon != std::string_view::npos) {
			portText = address.substr(colon + 1);
			address = address.substr(0, colon);
			hasPort = true;
		}
	} else if (address.front() == '[') {
		const auto close = address.find(']');
		if (close == std::string_view::npos)
			return detail::failSocket(10022);
		const auto suffix = address.substr(close + 1);
		if (!suffix.empty()) {
			if (suffix.front() != ':')
				return detail::failSocket(10022);
			hasPort = true;
			portText = suffix.substr(1);
		}
		address = address.substr(1, close - 1);
	}
	uint32_t port = 0;
	if (hasPort && (!decimal(portText, port) || !port || port > 65535))
		return detail::failSocket(10022);
	sockaddr_storage native{};
	if (family == AF_INET) {
		auto *ip = reinterpret_cast<sockaddr_in *>(&native);
		const std::string literal(address);
		ULONG value = 0;
		if (literal.find_first_not_of("0123456789abcdefABCDEFxX.") != std::string::npos ||
			!parseIpv4(literal.c_str(), value))
			return detail::failSocket(10022);
		ip->sin_family = AF_INET;
		ip->sin_port = htons(static_cast<USHORT>(port));
		ip->sin_addr.s_addr = value;
	} else {
		auto *ip = reinterpret_cast<sockaddr_in6 *>(&native);
		const auto percent = address.find('%');
		if (percent != std::string_view::npos) {
			uint32_t scope = 0;
			if (!decimal(address.substr(percent + 1), scope))
				return detail::failSocket(10022);
			ip->sin6_scope_id = scope;
			address = address.substr(0, percent);
		}
		const std::string literal(address);
		if (::inet_pton(AF_INET6, literal.c_str(), &ip->sin6_addr) != 1)
			return detail::failSocket(10022);
		ip->sin6_family = AF_INET6;
		ip->sin6_port = htons(static_cast<USHORT>(port));
	}
	const int error = detail::addressFromNative(reinterpret_cast<const sockaddr *>(&native), output, length);
	return error ? detail::failSocket(error) : 0;
}

LPSTR WINAPI inet_ntoa(ULONG address) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("inet_ntoa(0x%x)\n", address);
	thread_local auto buffer = wibo::heap::make_guest_unique<char[]>(16);
	if (!buffer) {
		detail::setLastError(10055);
		return nullptr;
	}
	std::snprintf(buffer.get(), 16, "%u.%u.%u.%u", address & 0xFFU, (address >> 8) & 0xFFU, (address >> 16) & 0xFFU,
				  address >> 24);
	return buffer.get();
}

int WINAPI getaddrinfo(LPCSTR node, LPCSTR service, const ADDRINFOA *hints, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("getaddrinfo(%s, %s, %p, %p)\n", node ? node : "(null)", service ? service : "(null)", hints, result);
	auto fail = [](int error) {
		detail::setLastError(error);
		return error;
	};
	if (!result)
		return fail(10014);
	*result = GUEST_NULL;
	if (!detail::requireStarted())
		return 10093;
	addrinfo hostHints{};
	if (hints) {
		if (hints->ai_addrlen || hints->ai_canonname || hints->ai_addr || hints->ai_next)
			return fail(11003);
		if (hints->ai_flags & ~kSupportedFlags)
			return fail(10022);
		if (!node && (hints->ai_flags & kCanonName))
			return fail(11003);
		if (hints->ai_family != 0 && hints->ai_family != AF_INET && hints->ai_family != kWinInet6)
			return fail(10047);
		hostHints.ai_family = hints->ai_family == kWinInet6 ? AF_INET6 : hints->ai_family;
		hostHints.ai_socktype = hints->ai_socktype;
		hostHints.ai_protocol = hints->ai_protocol;
		hostHints.ai_flags = nativeFlags(hints->ai_flags);
	}
	if (!asciiName(node) || !asciiName(service) || (node && (!*node || std::strcmp(node, "..localmachine") == 0)))
		return fail(10045);
	addrinfo *addresses = nullptr;
	const int status = ::getaddrinfo(node, service, &hostHints, &addresses);
	if (status)
		return fail(resolutionError(status));
	const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> hostOwner(addresses, ::freeaddrinfo);
	std::vector<ADDRINFOA *> nodes;
	for (const auto *host = addresses; host; host = host->ai_next) {
		const size_t addressSize = host->ai_family == AF_INET ? 16 : host->ai_family == AF_INET6 ? 28 : 0;
		if (!addressSize || !host->ai_addr)
			continue;
		const size_t nameSize = host->ai_canonname ? std::strlen(host->ai_canonname) + 1 : 0;
		auto *entry =
			static_cast<ADDRINFOA *>(wibo::heap::guestMalloc(sizeof(ADDRINFOA) + addressSize + nameSize, true));
		if (!entry) {
			for (auto *allocated : nodes)
				wibo::heap::guestFree(allocated);
			return fail(8);
		}
		entry->ai_flags = windowsFlags(host->ai_flags);
		entry->ai_family = host->ai_family == AF_INET ? AF_INET : kWinInet6;
		entry->ai_socktype = host->ai_socktype;
		entry->ai_protocol = host->ai_protocol;
		entry->ai_addrlen = addressSize;
		auto *address = reinterpret_cast<unsigned char *>(entry + 1);
		entry->ai_addr = toGuestPtr(address);
		int convertedSize = static_cast<int>(addressSize);
		const int converted = detail::addressFromNative(host->ai_addr, address, &convertedSize);
		if (converted) {
			wibo::heap::guestFree(entry);
			for (auto *allocated : nodes)
				wibo::heap::guestFree(allocated);
			return fail(converted);
		}
		if (nameSize) {
			entry->ai_canonname = toGuestPtr(address + addressSize);
			std::memcpy(address + addressSize, host->ai_canonname, nameSize);
		}
		if (!nodes.empty())
			nodes.back()->ai_next = toGuestPtr(entry);
		nodes.push_back(entry);
	}
	if (nodes.empty())
		return fail(11004);
	auto &registry = addressRegistry();
	{
		std::lock_guard lock(registry.mutex);
		for (auto *entry : nodes)
			registry.nodes.emplace(entry, fromGuestPtr<ADDRINFOA>(entry->ai_next));
	}
	*result = toGuestPtr(nodes.front());
	detail::setLastError(0);
	return 0;
}

void WINAPI freeaddrinfo(ADDRINFOA *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("freeaddrinfo(%p)\n", result);
	auto &registry = addressRegistry();
	std::lock_guard lock(registry.mutex);
	while (result) {
		const auto found = registry.nodes.find(result);
		if (found == registry.nodes.end())
			break;
		auto *next = found->second;
		registry.nodes.erase(found);
		wibo::heap::guestFree(result);
		result = next;
	}
}

} // namespace ws2
