#include "user32.h"

#include "common.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "kernel32/winnls.h"
#include "setup.h"
#include "strutil.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace {

constexpr size_t kPrintBufferUnits = 1024;

class PrintArguments {
  public:
	explicit PrintArguments(const void *cursor) : mCursor(static_cast<const uint8_t *>(cursor)) {}

	uint32_t integer() { return read<uint32_t>(); }
	uint64_t largeInteger() { return read<uint64_t>(); }
	GUEST_PTR pointer() { return read<GUEST_PTR>(); }

  private:
	template <typename T> T read() {
		T value;
		std::memcpy(&value, mCursor, sizeof(value));
		mCursor += std::max(sizeof(GUEST_PTR), sizeof(T));
		return value;
	}
	const uint8_t *mCursor;
};

struct PrintField {
	size_t width = 0;
	size_t precision = 0;
	bool left = false;
	bool prefix = false;
	bool zero = false;
	enum class Length { Normal, Short, Long, Wide, Pointer, Large } length = Length::Normal;
};

struct PrintOutput {
	std::array<WCHAR, kPrintBufferUnits> units{};
	size_t size = 0;

	[[nodiscard]] size_t remaining() const { return units.size() - 1 - size; }
	void append(WCHAR value) {
		if (remaining())
			units[size++] = value;
	}
	void pad(WCHAR value, size_t count) {
		while (count-- && remaining())
			append(value);
	}
};

size_t printDecimal(LPCWSTR format, size_t length, size_t &position) {
	uint32_t value = 0;
	while (position < length && format[position] >= '0' && format[position] <= '9') {
		// Field parsing wraps at the API's 32-bit integer width before output bounds apply.
		value = value * 10 + static_cast<uint32_t>(format[position++] - '0');
	}
	return value;
}

PrintField printField(LPCWSTR format, size_t length, size_t &position) {
	PrintField field;
	if (position < length && format[position] == '-') {
		field.left = true;
		++position;
	}
	if (position < length && format[position] == '#') {
		field.prefix = true;
		++position;
	}
	if (position < length && format[position] == '0') {
		field.zero = true;
		++position;
	}
	field.width = printDecimal(format, length, position);
	if (position < length && format[position] == '.') {
		++position;
		field.precision = printDecimal(format, length, position);
	}
	if (position < length) {
		switch (format[position]) {
		case 'h':
			field.length = PrintField::Length::Short;
			++position;
			break;
		case 'l':
			field.length = PrintField::Length::Long;
			++position;
			break;
		case 'w':
			field.length = PrintField::Length::Wide;
			++position;
			break;
		case 'I':
			++position;
			field.length = PrintField::Length::Pointer;
			if (length - position >= 2 && format[position] == '6' && format[position + 1] == '4') {
				field.length = PrintField::Length::Large;
				position += 2;
			} else if (length - position >= 2 && format[position] == '3' && format[position + 1] == '2') {
				field.length = PrintField::Length::Normal;
				position += 2;
			}
			break;
		default:
			break;
		}
	}
	return field;
}

void printNumber(PrintOutput &output, PrintArguments &arguments, PrintField field, WCHAR type) {
	if (type == 'p') {
		field.length = PrintField::Length::Pointer;
		field.width = 2 * sizeof(GUEST_PTR);
		field.zero = true;
	}
	const bool signedDecimal = type == 'd' || type == 'i';
	const bool hexadecimal = type == 'x' || type == 'X' || type == 'p';
	uint64_t value;
	bool negative = false;
	if (field.length == PrintField::Length::Large) {
		value = arguments.largeInteger();
		negative = signedDecimal && (value >> 63);
	} else if (field.length == PrintField::Length::Pointer) {
		value = arguments.pointer();
		negative = signedDecimal && (value >> (8 * sizeof(GUEST_PTR) - 1));
	} else {
		value = arguments.integer();
		negative = signedDecimal && (value >> 31);
	}
	if (negative) {
		value = uint64_t{0} - value;
		if (field.length != PrintField::Length::Large &&
			!(field.length == PrintField::Length::Pointer && sizeof(GUEST_PTR) == 8))
			value = static_cast<uint32_t>(value);
	}
	const char *digits = type == 'X' || type == 'p' ? "0123456789ABCDEF" : "0123456789abcdef";
	std::array<WCHAR, 20> reversed{};
	size_t count = 0;
	const unsigned int radix = hexadecimal ? 16 : 10;
	do {
		reversed[count++] = static_cast<uint8_t>(digits[value % radix]);
		value /= radix;
	} while (value);
	const size_t numberUnits = count + negative;
	field.width = std::min(field.width, output.remaining());
	size_t precision = std::max(field.precision, numberUnits);
	if (field.zero && !field.left)
		precision = std::max(precision, field.width);
	precision = std::min(precision, output.remaining());
	const size_t spaces = field.width > precision ? field.width - precision : 0;
	if (!field.left)
		output.pad(' ', spaces);
	if (hexadecimal && field.prefix) {
		output.append('0');
		output.append(type == 'X' || type == 'p' ? 'X' : 'x');
	}
	if (negative)
		output.append('-');
	output.pad('0', precision > numberUnits ? precision - numberUnits : 0);
	while (count)
		output.append(reversed[--count]);
	if (field.left)
		output.pad(' ', spaces);
}

bool narrowPrintField(PrintField field, WCHAR type) {
	switch (type) {
	case 'c':
	case 's':
		return field.length == PrintField::Length::Short;
	case 'C':
		return field.length != PrintField::Length::Long;
	default:
		return field.length != PrintField::Length::Long && field.length != PrintField::Length::Wide;
	}
}

bool printText(PrintOutput &output, PrintArguments &arguments, PrintField field, WCHAR type) {
	const bool character = type == 'c' || type == 'C';
	const bool narrow = narrowPrintField(field, type);
	const bool latin1 = !narrow || kernel32::GetACP() == 28591;
	std::array<WCHAR, kPrintBufferUnits> text{};
	size_t count = 0;
	if (character) {
		const uint32_t value = arguments.integer();
		if (narrow && static_cast<uint8_t>(value) >= 128 && !latin1)
			return false;
		text[count++] = narrow ? static_cast<uint8_t>(value) : static_cast<WCHAR>(value);
	} else {
		const auto pointer = static_cast<uintptr_t>(arguments.pointer());
		const size_t limit = std::min(output.remaining(), field.precision ? field.precision : output.remaining());
		if (!pointer) {
			constexpr char nullText[] = "(null)";
			while (count < limit && nullText[count]) {
				text[count] = static_cast<uint8_t>(nullText[count]);
				++count;
			}
		} else if (narrow) {
			const auto *source = reinterpret_cast<const uint8_t *>(pointer);
			while (count < limit && source[count]) {
				if (source[count] >= 128 && !latin1)
					return false;
				text[count] = source[count];
				++count;
			}
		} else {
			const auto *source = reinterpret_cast<LPCWSTR>(pointer);
			while (count < limit && source[count]) {
				text[count] = source[count];
				++count;
			}
		}
	}
	const size_t width = std::min(field.width, output.remaining());
	const size_t spaces = width > count ? width - count : 0;
	if (!field.left)
		output.pad(' ', spaces);
	for (size_t index = 0; index < count; ++index)
		output.append(text[index]);
	if (field.left)
		output.pad(' ', spaces);
	return true;
}

int printWide(LPWSTR buffer, LPCWSTR format, const void *cursor) {
	DEBUG_LOG("wsprintfW(%p, %p, %p)\n", buffer, format, cursor);
	if (!buffer || !format) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const DWORD incomingError = kernel32::getLastError();
	const size_t length = wstrnlen(format, 4096);
	if (length == 4096) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	PrintOutput output;
	PrintArguments arguments(cursor);
	size_t position = 0;
	while (position < length && output.remaining()) {
		const WCHAR value = format[position++];
		if (value != '%') {
			output.append(value);
			continue;
		}
		if (position < length && format[position] == '%') {
			output.append(format[position++]);
			continue;
		}
		const PrintField field = printField(format, length, position);
		if (position == length)
			break;
		const WCHAR type = format[position++];
		switch (type) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'p':
			printNumber(output, arguments, field, type);
			break;
		case 'c':
		case 'C':
		case 's':
		case 'S':
			// Latin-1 has an exact byte-to-code-unit mapping; other ANSI pages need conversion.
			if (!printText(output, arguments, field, type)) {
				kernel32::setLastError(ERROR_NOT_SUPPORTED);
				return 0;
			}
			break;
		default:
			// Unknown conversions are literal characters and consume no argument.
			if (!field.left) {
				const size_t width = std::min(field.width, output.remaining());
				output.pad(' ', width > field.precision ? width - field.precision : 0);
			}
			output.append(type);
			break;
		}
	}
	std::memcpy(buffer, output.units.data(), (output.size + 1) * sizeof(WCHAR));
	kernel32::setLastError(incomingError);
	return output.remaining() ? static_cast<int>(output.size) : static_cast<int>(kPrintBufferUnits);
}

} // namespace

namespace user32 {

#ifdef WIBO_GUEST_64
int CDECL_NO_CONV wsprintfW(LPWSTR buffer, LPCWSTR format, ...) {
	__builtin_ms_va_list arguments;
	__builtin_ms_va_start(arguments, format);
#if defined(__APPLE__)
	TEB *teb = enterHostContext();
#endif
	const int result = printWide(buffer, format, arguments);
	__builtin_ms_va_end(arguments);
#if defined(__APPLE__)
	enterGuestContext(teb);
#endif
	return result;
}
#else
int CDECL wsprintfW(LPWSTR buffer, LPCWSTR format, const void *arguments) {
	return printWide(buffer, format, arguments);
}
#endif

} // namespace user32
