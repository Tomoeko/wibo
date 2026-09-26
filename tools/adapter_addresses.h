#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {
// Preserve the provider's SDK layout and replace its pointers with checked offsets.
// The consumer must have the same pointer width as the provider image.
class AdapterSnapshot {
	std::vector<BYTE> &source;
	std::unordered_set<size_t> pointerFields;

	bool range(const void *pointer, size_t bytes, size_t &offset) const {
		const auto base = reinterpret_cast<uintptr_t>(source.data());
		const auto address = reinterpret_cast<uintptr_t>(pointer);
		if (address < base || address - base > source.size())
			return false;
		offset = address - base;
		return bytes <= source.size() - offset;
	}

	template <typename Pointer> bool pointer(Pointer *field, size_t bytes = 1) {
		size_t position = 0, target = 0;
		if (!range(field, sizeof(*field), position))
			return false;
		if (!*field)
			return bytes <= 1;
		if (!range(*field, bytes, target) || target == 0)
			return false;
		if (pointerFields.insert(position).second) {
			const auto relative = static_cast<uintptr_t>(target);
			std::memcpy(data.data() + position, &relative, sizeof(relative));
			relocations.push_back(static_cast<uint32_t>(position));
		}
		return true;
	}

	template <typename Character> bool string(Character **field) {
		if (!*field)
			return true;
		size_t offset = 0;
		if (!range(*field, sizeof(Character), offset))
			return false;
		const size_t available = (source.size() - offset) / sizeof(Character);
		for (size_t index = 0; index < available; ++index) {
			Character value;
			std::memcpy(&value, source.data() + offset + index * sizeof(Character), sizeof(value));
			if (value == 0)
				return pointer(field, (index + 1) * sizeof(Character));
		}
		return false;
	}

	bool socketAddress(SOCKET_ADDRESS &address) {
		if (address.iSockaddrLength < 0 || address.iSockaddrLength > static_cast<int>(sizeof(SOCKADDR_STORAGE)))
			return false;
		if (!address.lpSockaddr)
			return address.iSockaddrLength == 0;
		return address.iSockaddrLength >= static_cast<int>(sizeof(USHORT)) &&
			   pointer(&address.lpSockaddr, static_cast<size_t>(address.iSockaddrLength));
	}

	template <typename Node> bool addresses(Node **first) {
		if (!pointer(first))
			return false;
		std::unordered_set<const Node *> visited;
		for (auto *node = *first; node; node = node->Next) {
			size_t offset = 0;
			constexpr size_t minimum = offsetof(Node, Address) + sizeof(SOCKET_ADDRESS);
			if (visited.size() >= 65536 || !visited.insert(node).second || !range(node, minimum, offset) ||
				node->Length < minimum || !range(node, node->Length, offset) || !pointer(&node->Next) ||
				!socketAddress(node->Address))
				return false;
		}
		return true;
	}

	bool suffixes(IP_ADAPTER_DNS_SUFFIX **first) {
		if (!pointer(first))
			return false;
		std::unordered_set<const IP_ADAPTER_DNS_SUFFIX *> visited;
		for (auto *node = *first; node; node = node->Next) {
			size_t offset = 0;
			if (visited.size() >= 65536 || !visited.insert(node).second || !range(node, sizeof(*node), offset) ||
				!pointer(&node->Next))
				return false;
		}
		return true;
	}

  public:
	std::vector<BYTE> data;
	std::vector<uint32_t> relocations;
	explicit AdapterSnapshot(std::vector<BYTE> &source) : source(source), data(source) {}

	bool capture() {
		std::unordered_set<const IP_ADAPTER_ADDRESSES *> visited;
		for (auto *node = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(source.data()); node; node = node->Next) {
			size_t offset = 0;
			constexpr size_t minimum = offsetof(IP_ADAPTER_ADDRESSES, FirstPrefix) + sizeof(void *);
			if (visited.size() >= 4096 || !visited.insert(node).second || !range(node, minimum, offset) ||
				node->Length < minimum || !range(node, node->Length, offset) ||
				node->PhysicalAddressLength > sizeof(node->PhysicalAddress) || !pointer(&node->Next) ||
				!string(&node->AdapterName) || !string(&node->DnsSuffix) || !string(&node->Description) ||
				!string(&node->FriendlyName) || !addresses(&node->FirstUnicastAddress) ||
				!addresses(&node->FirstAnycastAddress) || !addresses(&node->FirstMulticastAddress) ||
				!addresses(&node->FirstDnsServerAddress) || !addresses(&node->FirstPrefix))
				return false;
			if (node->Length >= offsetof(IP_ADAPTER_ADDRESSES, Ipv4Metric) &&
				(!addresses(&node->FirstWinsServerAddress) || !addresses(&node->FirstGatewayAddress)))
				return false;
			if (node->Length >= offsetof(IP_ADAPTER_ADDRESSES, CompartmentId) && !socketAddress(node->Dhcpv4Server))
				return false;
			if (node->Length >= offsetof(IP_ADAPTER_ADDRESSES, Dhcpv6ClientDuid) && !socketAddress(node->Dhcpv6Server))
				return false;
			if (node->Length >= sizeof(IP_ADAPTER_ADDRESSES) && !suffixes(&node->FirstDnsSuffix))
				return false;
		}
		std::sort(relocations.begin(), relocations.end());
		return true;
	}
};

template <typename Response> bool adapterAddresses(ULONG family, ULONG flags, ULONG width) {
	if (width != sizeof(void *)) {
		Response response;
		response.header(ERROR_NOT_SUPPORTED);
		return response.write();
	}
	ULONG size = 0;
	DWORD status = GetAdaptersAddresses(family, flags, nullptr, nullptr, &size);
	std::vector<BYTE> storage;
	for (unsigned attempt = 0; status == ERROR_BUFFER_OVERFLOW && attempt != 3; ++attempt) {
		if (size < sizeof(IP_ADAPTER_ADDRESSES_XP) || size > 2 * 1024 * 1024) {
			status = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}
		storage.assign(size, 0);
		status = GetAdaptersAddresses(family, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()),
									  &size);
	}
	AdapterSnapshot snapshot(storage);
	if (status == NO_ERROR && (storage.empty() || !snapshot.capture() ||
							   snapshot.data.size() + snapshot.relocations.size() * 4 > 8 * 1024 * 1024 - 24))
		status = ERROR_INVALID_DATA;
	Response response;
	response.header(status);
	if (status == ERROR_BUFFER_OVERFLOW)
		response.number(size);
	if (status == NO_ERROR) {
		response.number(sizeof(void *));
		response.bytes(snapshot.data.data(), snapshot.data.size());
		response.number(static_cast<uint32_t>(snapshot.relocations.size()));
		for (auto offset : snapshot.relocations)
			response.number(offset);
	}
	return response.write();
}
} // namespace
