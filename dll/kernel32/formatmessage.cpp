#include "kernel32/winbase.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "message_format.h"
#include "strutil.h"
#include "system_provider.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string_view>

namespace {
constexpr DWORD kAllocate = 0x100;
constexpr DWORD kIgnoreInserts = 0x200;
constexpr DWORD kFromSystem = 0x1000;
constexpr DWORD kArgumentArray = 0x2000;
constexpr DWORD kSupportedFlags = 0xFF | kAllocate | kIgnoreInserts | kFromSystem | kArgumentArray;

constexpr DWORD kFromString = 0x400;
constexpr size_t kSourceUnits = 4096;
constexpr size_t kRequestBytes = 64 * 1024;
constexpr size_t kMessageBytes = 128 * 1024;

void appendNumber(std::vector<uint8_t> &bytes, uint32_t value) {
	for (unsigned shift = 0; shift != 32; shift += 8)
		bytes.push_back(static_cast<uint8_t>(value >> shift));
}

struct MessageArgument {
	uint32_t kind;
	uint32_t number = 0;
	std::vector<uint8_t> text;
};

class MessageArguments {
  public:
	MessageArguments(LPCVOID arguments, bool array) : mArguments(arguments), mArray(array) {}
	DWORD get(unsigned number, bool large, uint64_t &value) {
		if (!mArguments)
			return ERROR_INVALID_PARAMETER;
		if (number > 99 || (large && mArray))
			return ERROR_NOT_SUPPORTED;
		if (mArray) {
			GUEST_PTR pointer;
			std::memcpy(&pointer, static_cast<const uint8_t *>(mArguments) + (number - 1) * sizeof(pointer),
						sizeof(pointer));
			value = pointer;
			mLast = std::max(mLast, number);
			return ERROR_SUCCESS;
		}
		if (!mCursor) {
			GUEST_PTR cursor;
			std::memcpy(&cursor, mArguments, sizeof(cursor));
			if (!cursor)
				return ERROR_INVALID_PARAMETER;
			mCursor = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(cursor));
		}
		while (mLast < number) {
			const size_t bytes = large ? sizeof(uint64_t) : sizeof(GUEST_PTR);
			uint64_t next = 0;
			std::memcpy(&next, mCursor, bytes);
			mCursor += bytes;
			mValues[mLast++] = next;
		}
		value = mValues[number - 1];
		return ERROR_SUCCESS;
	}
	DWORD field(unsigned insert, unsigned stars, uint64_t *values) {
		if (mArray) {
			for (unsigned index = 0; index <= stars; ++index) {
				const DWORD status = get(index ? mLast + 1 : insert, false, values[index]);
				if (status)
					return status;
			}
			return ERROR_SUCCESS;
		}
		uint64_t unused = 0;
		if (insert > 1) {
			const DWORD status = get(insert - 1, false, unused);
			if (status)
				return status;
		}
		for (unsigned index = 0; index != stars; ++index) {
			const DWORD status = get(index ? mLast + 1 : insert, false, values[index]);
			if (status)
				return status;
		}
		// Width and precision advance the physical va_list but collapse one logical insertion slot.
		if (stars)
			--mLast;
		return get(stars ? mLast + 1 : insert, false, values[stars]);
	}

  private:
	LPCVOID mArguments;
	bool mArray;
	const uint8_t *mCursor = nullptr;
	std::array<uint64_t, 99> mValues{};
	unsigned mLast = 0;
};

struct MessageFormat {
	std::vector<uint8_t> source;
	std::vector<uint8_t> arguments;
	bool plain = true;
};

DWORD prepareMessage(LPCVOID source, bool wide, DWORD flags, LPCVOID arguments, size_t byteBudget,
					 MessageFormat &message) {
	if (!source)
		return ERROR_INVALID_PARAMETER;
	const size_t length = wide ? wstrnlen(static_cast<LPCWSTR>(source), kSourceUnits)
							   : strnlen(static_cast<LPCSTR>(source), kSourceUnits);
	if (length == kSourceUnits)
		return ERROR_NOT_ENOUGH_MEMORY;
	const auto character = [&](size_t index) {
		return wide ? static_cast<LPCWSTR>(source)[index] : WCHAR(static_cast<const uint8_t *>(source)[index]);
	};
	const auto append = [&](WCHAR value) {
		message.source.push_back(static_cast<uint8_t>(value));
		if (wide)
			message.source.push_back(static_cast<uint8_t>(value >> 8));
	};
	const auto appendAscii = [&](std::string_view text) {
		for (unsigned char value : text)
			append(value);
	};
	std::vector<MessageArgument> fields;
	size_t argumentBytes = 4;
	// Reserve space for the original source and the maximum rewritten field annotations.
	const size_t sourceReserve = length * (wide ? sizeof(WCHAR) : 1) + 99 * 34 * (wide ? sizeof(WCHAR) : 1);
	if (sourceReserve + argumentBytes > byteBudget)
		return ERROR_NOT_ENOUGH_MEMORY;
	size_t projectedUnits = length * 3;
	MessageArguments reader(arguments, flags & kArgumentArray);
	bool largeSeen = false, insertionSeen = false;
	for (size_t position = 0; position < length;) {
		const WCHAR value = character(position++);
		if (value == '\r' || value == '\n' || value == '%' || (!wide && value >= 128))
			message.plain = false;
		if (value != '%' || (flags & kIgnoreInserts)) {
			append(value);
			continue;
		}
		if (position == length)
			return ERROR_INVALID_PARAMETER;
		if (character(position) == '0') {
			append('%');
			append('0');
			break;
		}
		if (character(position) < '1' || character(position) > '9') {
			append('%');
			append(character(position++));
			continue;
		}
		unsigned insert = character(position++) - '0';
		if (position < length && character(position) >= '0' && character(position) <= '9')
			insert = insert * 10 + character(position++) - '0';
		std::string specification = "s";
		if (position < length && character(position) == '!') {
			specification.clear();
			++position;
			while (position < length && character(position) != '!') {
				if (character(position) >= 128 || specification.size() >= 30)
					return ERROR_NOT_SUPPORTED;
				specification.push_back(static_cast<char>(character(position++)));
			}
			if (position == length)
				return ERROR_INVALID_PARAMETER;
			++position;
		}
		if (specification == "I64u" || specification == "I64d") {
			if ((flags & kArgumentArray) || insert != 1 || insertionSeen)
				return ERROR_NOT_SUPPORTED;
			uint64_t number = 0;
			const DWORD status = reader.get(1, true, number);
			if (status)
				return status;
			char text[32];
			const auto conversion = specification.back() == 'u'
										? std::to_chars(text, text + sizeof(text), number)
										: std::to_chars(text, text + sizeof(text), static_cast<int64_t>(number));
			appendAscii(std::string_view(text, conversion.ptr - text));
			largeSeen = insertionSeen = true;
			continue;
		}
		if (largeSeen || specification.empty())
			return ERROR_NOT_SUPPORTED;
		insertionSeen = true;
		wibo::message::Field parsed;
		if (!wibo::message::parseField(specification, parsed))
			return ERROR_NOT_SUPPORTED;
		const unsigned stars = parsed.stars();
		const bool string = parsed.string();
		uint64_t values[3]{};
		const DWORD status = reader.field(insert, stars, values);
		if (status)
			return status;
		if (fields.size() + stars + 1 > 99)
			return ERROR_NOT_ENOUGH_MEMORY;
		append('%');
		appendAscii(std::to_string(fields.size() + 1));
		append('!');
		appendAscii(specification);
		append('!');
		for (unsigned index = 0; index != stars; ++index)
			fields.push_back({0, static_cast<uint32_t>(values[index]), {}});
		MessageArgument field{0, static_cast<uint32_t>(values[stars]), {}};
		const size_t fieldBytes = (stars + 1) * 8;
		if (sourceReserve + argumentBytes + fieldBytes > byteBudget)
			return ERROR_NOT_ENOUGH_MEMORY;
		if (string) {
			const bool stringWide = parsed.wideString(wide);
			if (!values[stars])
				return ERROR_NOT_SUPPORTED;
			const auto *text = reinterpret_cast<const void *>(static_cast<uintptr_t>(values[stars]));
			const size_t stringUnit = stringWide ? sizeof(WCHAR) : 1;
			const size_t limit =
				std::min(kSourceUnits, (byteBudget - sourceReserve - argumentBytes - fieldBytes) / stringUnit + 1);
			const size_t units =
				stringWide ? wstrnlen(static_cast<LPCWSTR>(text), limit) : strnlen(static_cast<LPCSTR>(text), limit);
			if (units == limit)
				return ERROR_NOT_ENOUGH_MEMORY;
			field.kind = stringWide ? 1 : 2;
			field.text.resize(units * (stringWide ? sizeof(WCHAR) : 1));
			if (!field.text.empty())
				std::memcpy(field.text.data(), text, field.text.size());
		}
		uint32_t dimensions[2]{};
		for (unsigned index = 0; index != stars; ++index)
			dimensions[index] = static_cast<uint32_t>(values[index]);
		size_t extent = 0;
		if (!wibo::message::extent(parsed, dimensions, field.text.size() / (field.kind == 1 ? sizeof(WCHAR) : 1),
								   extent) ||
			projectedUnits + extent * 3 > kMessageBytes / (wide ? sizeof(WCHAR) : 4))
			return ERROR_NOT_SUPPORTED;
		projectedUnits += extent * 3;
		argumentBytes += fieldBytes + field.text.size();
		fields.push_back(std::move(field));
	}
	appendNumber(message.arguments, static_cast<uint32_t>(fields.size()));
	for (const auto &field : fields) {
		appendNumber(message.arguments, field.kind);
		appendNumber(message.arguments, field.kind ? static_cast<uint32_t>(field.text.size()) : field.number);
		message.arguments.insert(message.arguments.end(), field.text.begin(), field.text.end());
	}
	return ERROR_SUCCESS;
}

std::string encodeMessageBytes(const std::vector<uint8_t> &bytes) {
	if (bytes.empty())
		return "-";
	return wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

DWORD formatStringMessage(DWORD flags, LPCVOID source, LPVOID buffer, DWORD size, LPCVOID arguments, bool wide) {
	const DWORD incomingError = kernel32::getLastError();
	const auto fail = [](DWORD error) {
		kernel32::setLastError(error);
		return DWORD(0);
	};
	if (!buffer || !source)
		return fail(ERROR_INVALID_PARAMETER);
	if (flags & ~(kSupportedFlags | kFromString))
		return fail(ERROR_NOT_SUPPORTED);
	const bool allocated = flags & kAllocate;
	const size_t unit = wide ? sizeof(WCHAR) : 1;
	const uint64_t seedBytes = allocated ? 0 : uint64_t(size) * unit;
	if (seedBytes > (kRequestBytes - 256) / 2 || (allocated && size > 1024 * 1024 / unit))
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	MessageFormat message;
	DWORD status = prepareMessage(source, wide, flags, arguments, (kRequestBytes - 256) / 2 - seedBytes, message);
	if (status) {
		if (allocated && status == ERROR_INVALID_PARAMETER) {
			const GUEST_PTR pointer = 0;
			std::memcpy(buffer, &pointer, sizeof(pointer));
		}
		return fail(status);
	}
	if (message.source.size() / unit >= kSourceUnits)
		return fail(ERROR_NOT_ENOUGH_MEMORY);
	std::vector<uint8_t> output;
	uint32_t result = 0, nativeError = incomingError;
	if (message.plain && !(flags & 0xff)) {
		const size_t characters = message.source.size() / unit;
		if (!characters) {
			nativeError = 235;
			if (!allocated && wide && size)
				output = {0, 0};
		} else if (!allocated && size <= characters) {
			nativeError = ERROR_INSUFFICIENT_BUFFER;
			if (wide && size) {
				output.assign(message.source.begin(),
							  message.source.begin() + static_cast<ptrdiff_t>((size - 1) * unit));
				output.resize(size * unit, 0);
			}
		} else {
			result = static_cast<uint32_t>(characters);
			output = message.source;
			output.resize(output.size() + unit, 0);
		}
	} else {
		if (message.source.size() + message.arguments.size() + seedBytes > (kRequestBytes - 256) / 2)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		std::vector<uint8_t> seed(static_cast<size_t>(seedBytes));
		if (!seed.empty())
			std::memcpy(seed.data(), buffer, seed.size());
		std::vector<uint8_t> response;
		if (!wibo::provider::request({"format-message-string", wide ? "w" : "a", std::to_string(flags),
									  std::to_string(size), encodeMessageBytes(message.source),
									  encodeMessageBytes(message.arguments), encodeMessageBytes(seed),
									  std::to_string(incomingError)},
									 response))
			return fail(ERROR_NOT_SUPPORTED);
		wibo::provider::Reader reader(response);
		int32_t transportStatus = 0;
		if (!reader.header(transportStatus))
			return fail(13);
		if (transportStatus) {
			if (!reader.done())
				return fail(ERROR_INVALID_DATA);
			if (transportStatus == wibo::provider::kUnavailable)
				return fail(ERROR_NOT_SUPPORTED);
			return fail(transportStatus < 0 ? ERROR_INVALID_DATA : static_cast<DWORD>(transportStatus));
		}
		if (!reader.number(result) || !reader.number(nativeError) || !reader.bytes(output) || !reader.done() ||
			output.size() % unit || output.size() > kMessageBytes ||
			(allocated ? (result ? output.size() != (uint64_t(result) + 1) * unit : !output.empty())
					   : output.size() != seedBytes) ||
			(result && (!allocated && result >= size)))
			return fail(13);
		if (result) {
			const size_t terminator = result * unit;
			if (output[terminator] || (wide && output[terminator + 1]))
				return fail(13);
		}
	}
	if (allocated) {
		GUEST_PTR pointer = 0;
		if (result) {
			const size_t allocation = std::max<size_t>(size, result + 1);
			void *storage = wibo::heap::guestMalloc(allocation * unit, true);
			if (!storage)
				return fail(ERROR_NOT_ENOUGH_MEMORY);
			std::memcpy(storage, output.data(), output.size());
			pointer = toGuestPtr(storage);
		}
		std::memcpy(buffer, &pointer, sizeof(pointer));
	} else if (!output.empty()) {
		std::memcpy(buffer, output.data(), output.size());
	}
	kernel32::setLastError(nativeError);
	return result;
}

DWORD formatSystemMessage(DWORD flags, DWORD messageId, DWORD language, LPVOID buffer, DWORD size, LPCVOID arguments,
						  bool wide) {
	auto fail = [](DWORD error) {
		kernel32::setLastError(error);
		return DWORD(0);
	};
	if (!buffer)
		return fail(ERROR_INVALID_PARAMETER);
	if (!(flags & kFromSystem) || (flags & ~kSupportedFlags) || (arguments && !(flags & kIgnoreInserts)))
		return fail(ERROR_NOT_SUPPORTED);
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"format-message", wide ? "w" : "a", std::to_string(flags & ~kAllocate),
								  std::to_string(messageId), std::to_string(language)},
								 response))
		return fail(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return fail(13);
	if (status)
		return fail(reader.done() ? static_cast<DWORD>(status) : 13);
	std::vector<uint8_t> message;
	const size_t unit = wide ? sizeof(WCHAR) : 1;
	if (!reader.bytes(message) || !reader.done() || message.empty() || message.size() % unit ||
		message.size() > 128 * 1024)
		return fail(13);
	const size_t characters = message.size() / unit;
	if (flags & kAllocate) {
		const size_t allocation = std::max<size_t>(size, characters + 1);
		if (allocation > 1024 * 1024 / unit)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		void *allocated = wibo::heap::guestMalloc(allocation * unit, true);
		if (!allocated)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		std::memcpy(allocated, message.data(), message.size());
		*static_cast<GUEST_PTR *>(buffer) = toGuestPtr(allocated);
	} else {
		if (size <= characters)
			return fail(ERROR_INSUFFICIENT_BUFFER);
		std::memcpy(buffer, message.data(), message.size());
		std::memset(static_cast<uint8_t *>(buffer) + message.size(), 0, unit);
	}
	return static_cast<DWORD>(characters);
}
} // namespace

namespace kernel32 {
DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID source, DWORD messageId, DWORD language, LPSTR buffer, DWORD size,
							LPCVOID arguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FormatMessageA(0x%x, %p, %u, %u, %p, %u, %p)\n", flags, source, messageId, language, buffer, size,
			  arguments);
	return flags & kFromString ? formatStringMessage(flags, source, buffer, size, arguments, false)
							   : formatSystemMessage(flags, messageId, language, buffer, size, arguments, false);
}
DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID source, DWORD messageId, DWORD language, LPWSTR buffer, DWORD size,
							LPCVOID arguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FormatMessageW(0x%x, %p, %u, %u, %p, %u, %p)\n", flags, source, messageId, language, buffer, size,
			  arguments);
	return flags & kFromString ? formatStringMessage(flags, source, buffer, size, arguments, true)
							   : formatSystemMessage(flags, messageId, language, buffer, size, arguments, true);
}
} // namespace kernel32
