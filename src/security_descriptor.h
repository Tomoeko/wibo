#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace wibo::security {

// Self-relative descriptors use DWORD offsets on both guest architectures.
inline bool validRelativeDescriptor(std::span<const uint8_t> data) {
	if (data.size() < 20)
		return false;
	const auto word = [&data](size_t offset) -> uint16_t {
		return uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8);
	};
	const auto dword = [&data](size_t offset) {
		return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) | (uint32_t(data[offset + 2]) << 16) |
			   (uint32_t(data[offset + 3]) << 24);
	};
	if (data[0] != 1 || !(word(2) & 0x8000))
		return false;
	const auto validSid = [&data](size_t offset, size_t end) {
		return offset <= end && end - offset >= 8 && data[offset] == 1 && data[offset + 1] <= 15 &&
			   8 + 4 * size_t(data[offset + 1]) <= end - offset;
	};
	for (size_t field : {size_t(4), size_t(8)}) {
		const size_t offset = dword(field);
		if (offset && (offset < 20 || offset % 4 || !validSid(offset, data.size())))
			return false;
	}
	for (size_t field : {size_t(12), size_t(16)}) {
		const size_t offset = dword(field);
		if (!offset)
			continue;
		if (offset < 20 || offset % 4 || offset > data.size() || data.size() - offset < 8)
			return false;
		const size_t size = word(offset + 2);
		if (size < 8 || size > data.size() - offset || data[offset] < 2 || data[offset] > 4)
			return false;
		size_t cursor = offset + 8;
		const size_t end = offset + size;
		for (unsigned index = 0; index < word(offset + 4); ++index) {
			if (end - cursor < 4)
				return false;
			const size_t aceSize = word(cursor + 2);
			if (aceSize < 4 || aceSize % 4 || aceSize > end - cursor)
				return false;
			const unsigned type = data[cursor];
			if (type <= 3 || (type >= 9 && type <= 21 && type != 11 && type != 12 && type != 15 && type != 16)) {
				if (aceSize < 16 || !validSid(cursor + 8, cursor + aceSize))
					return false;
			} else if ((type >= 5 && type <= 8) || type == 11 || type == 12 || type == 15 || type == 16) {
				if (aceSize < 20)
					return false;
				const uint32_t flags = dword(cursor + 8);
				const size_t sidOffset = cursor + 12 + (flags & 1 ? 16 : 0) + (flags & 2 ? 16 : 0);
				if (flags & ~uint32_t(3) || !validSid(sidOffset, cursor + aceSize))
					return false;
			}
			cursor += aceSize;
		}
	}
	return true;
}

} // namespace wibo::security
