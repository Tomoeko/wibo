#include "strutil.h"

#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

std::optional<std::u16string> utf8ToUtf16(std::string_view name) {
	std::u16string result;
	for (size_t i = 0; i < name.size();) {
		uint32_t codePoint = static_cast<unsigned char>(name[i++]);
		unsigned continuation = 0;
		uint32_t minimum = 0;
		if (codePoint >= 0xc2 && codePoint <= 0xdf) {
			continuation = 1;
			codePoint &= 0x1f;
			minimum = 0x80;
		} else if (codePoint >= 0xe0 && codePoint <= 0xef) {
			continuation = 2;
			codePoint &= 0xf;
			minimum = 0x800;
		} else if (codePoint >= 0xf0 && codePoint <= 0xf4) {
			continuation = 3;
			codePoint &= 7;
			minimum = 0x10000;
		} else if (codePoint >= 0x80) {
			return std::nullopt;
		}
		if (continuation > name.size() - i)
			return std::nullopt;
		for (unsigned j = 0; j < continuation; ++j) {
			uint32_t next = static_cast<unsigned char>(name[i++]);
			if ((next & 0xc0) != 0x80)
				return std::nullopt;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
			return std::nullopt;
		if (codePoint < 0x10000) {
			result.push_back(static_cast<char16_t>(codePoint));
		} else {
			codePoint -= 0x10000;
			result.push_back(static_cast<char16_t>(0xd800 + (codePoint >> 10)));
			result.push_back(static_cast<char16_t>(0xdc00 + (codePoint & 0x3ff)));
		}
	}
	return result;
}

bool utf16ToUtf8(std::u16string_view input, std::string &output) {
	output.clear();
	for (size_t i = 0; i < input.size(); ++i) {
		uint32_t cp = input[i];
		if (!cp) {
			return false; // Host paths and process arguments cannot represent embedded NULs.
		}
		if (cp >= 0xD800 && cp <= 0xDBFF) {
			if (++i == input.size() || input[i] < 0xDC00 || input[i] > 0xDFFF) {
				return false;
			}
			cp = 0x10000 + ((cp - 0xD800) << 10) + input[i] - 0xDC00;
		} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
			return false;
		}
		if (cp < 0x80) {
			output.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			output.push_back(static_cast<char>(0x80 | (cp & 63)));
		} else if (cp < 0x10000) {
			output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
			output.push_back(static_cast<char>(0x80 | (cp & 63)));
		} else {
			output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
			output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
			output.push_back(static_cast<char>(0x80 | (cp & 63)));
		}
	}
	return true;
}

void toLowerInPlace(std::string &str) {
	std::transform(str.begin(), str.end(), str.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
}

void toUpperInPlace(std::string &str) {
	std::transform(str.begin(), str.end(), str.begin(),
				   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
}

std::string stringToLower(std::string_view str) {
	std::string result(str);
	toLowerInPlace(result);
	return result;
}

std::string stringToUpper(std::string_view str) {
	std::string result(str);
	toUpperInPlace(result);
	return result;
}

uint16_t wcharToLower(uint16_t ch) {
	if (ch >= 'A' && ch <= 'Z') {
		return static_cast<uint16_t>(ch + ('a' - 'A'));
	}
	wint_t wide = static_cast<wint_t>(ch);
	wint_t lowered = std::towlower(wide);
	if (lowered > 0xFFFF) {
		return ch;
	}
	return static_cast<uint16_t>(lowered);
}

uint16_t wcharToUpper(uint16_t ch) {
	if (ch >= 'a' && ch <= 'z') {
		return static_cast<uint16_t>(ch - ('a' - 'A'));
	}
	wint_t wide = static_cast<wint_t>(ch);
	wint_t uppered = std::towupper(wide);
	if (uppered > 0xFFFF) {
		return ch;
	}
	return static_cast<uint16_t>(uppered);
}

size_t wstrlen(const uint16_t *str) {
	if (!str)
		return 0;

	size_t len = 0;
	while (str[len] != 0)
		++len;
	return len;
}

size_t wstrnlen(const uint16_t *str, size_t numberOfElements) {
	if (!str)
		return 0;
	size_t len = 0;
	while (len < numberOfElements && str[len] != 0)
		++len;
	return len;
}

int wstrncmp(const uint16_t *string1, const uint16_t *string2, size_t count) {
	const uint16_t *ptr1 = string1;
	const uint16_t *ptr2 = string2;
	for (size_t i = 0; i < count; i++) {
		uint16_t c1 = *ptr1++;
		uint16_t c2 = *ptr2++;
		if (c1 != c2) {
			return (c1 > c2) ? 1 : -1;
		}
	}
	return 0;
}

const uint16_t *wstrstr(const uint16_t *dest, const uint16_t *src) {
	if (!*src)
		return dest;

	for (; *dest != 0; dest++) {
		const uint16_t *d = dest;
		const uint16_t *s = src;

		while (*d != 0 && *s != 0 && *d == *s) {
			d++;
			s++;
		}

		if (*s == 0) {
			return dest;
		}
	}

	return nullptr;
}

uint16_t *wstrchr(const uint16_t *str, uint16_t c) {
	if (!str)
		return nullptr;
	for (; *str != 0; str++) {
		if (*str == c) {
			return (uint16_t *)str;
		}
	}
	// If searching for '\0', return pointer to terminator
	if (c == 0) {
		return (uint16_t *)str;
	}
	return nullptr;
}

uint16_t *wstrrchr(const uint16_t *str, uint16_t c) {
	if (!str)
		return nullptr;
	const uint16_t *last = nullptr;
	const uint16_t *it = str;
	for (; *it != 0; ++it) {
		if (*it == c) {
			last = it;
		}
	}
	if (c == 0)
		return (uint16_t *)it;
	return (uint16_t *)last;
}

uint16_t *wstrcat(uint16_t *dest, const uint16_t *src) {
	uint16_t *d = dest;
	while (*d)
		d++;
	while ((*d++ = *src++) != 0)
		;
	return dest;
}

uint16_t *wstrncat(uint16_t *dest, const uint16_t *src, size_t count) {
	uint16_t *d = dest;
	while (*d)
		d++;
	for (size_t i = 0; i < count && src[i] != 0; i++) {
		*d++ = src[i];
	}
	*d = 0;
	return dest;
}

uint16_t *wstrcpy(uint16_t *dest, const uint16_t *src) {
	uint16_t *d = dest;
	while ((*d++ = *src++) != 0)
		;
	return dest;
}

size_t wstrncpy(uint16_t *dst, const uint16_t *src, size_t n) {
	if (!dst || !src || n == 0)
		return 0;
	size_t i = 0;
	for (; i < n && src[i] != 0; ++i) {
		dst[i] = src[i];
	}
	for (size_t j = i; j < n; ++j) {
		dst[j] = 0;
	}
	return i;
}

std::string wideStringToString(const uint16_t *src, int len) {
	if (!src)
		return {};

	size_t count;
	if (len >= 0) {
		count = static_cast<size_t>(len);
	} else {
		count = wstrlen(src);
	}

#ifndef NDEBUG
	std::stringstream hexDump;
	hexDump << std::hex;
	bool sawWide = false;
#endif

	std::string result(count, '\0');
	for (size_t i = 0; i < count; ++i) {
		uint16_t value = src[i];
#ifndef NDEBUG
		if (i > 0)
			hexDump << ' ';
		hexDump << "0x" << value;
		if (value > 0xFF)
			sawWide = true;
#endif
		result[i] = static_cast<char>(value & 0xFF);
	}

#ifndef NDEBUG
	if (sawWide) {
		size_t loggedLength = (len >= 0) ? count : wstrlen(src);
		DEBUG_LOG("wideString (%zu): %s\n", loggedLength, hexDump.str().c_str());
	}
#endif

	return result;
}

std::vector<uint16_t> stringToWideString(const char *src, size_t length) {
	if (!src) {
		return std::vector<uint16_t>{0};
	}
	size_t len = length == static_cast<size_t>(-1) ? std::strlen(src) : length;
	std::vector<uint16_t> res(len + 1);
	for (size_t i = 0; i < len; ++i) {
		res[i] = static_cast<uint16_t>(static_cast<unsigned char>(src[i]));
	}
	res[len] = 0; // ensure NUL termination
	return res;
}

std::u16string stringToUtf16(std::string_view str) {
	std::u16string result;
	result.reserve(str.size());
	for (unsigned char ch : str) {
		result.push_back(static_cast<char16_t>(ch));
	}
	return result;
}

long wstrtol(const uint16_t *string, uint16_t **end_ptr, int base) {
	if (!string) {
		if (end_ptr)
			*end_ptr = nullptr;
		return 0;
	}

	std::string normal_str = wideStringToString(string);
	char *normal_end = nullptr;
	long res = std::strtol(normal_str.c_str(), &normal_end, base);

	if (end_ptr) {
		if (normal_end && *normal_end) {
			size_t offset = normal_end - normal_str.c_str();
			*end_ptr = (uint16_t *)(string + offset);
		} else {
			*end_ptr = (uint16_t *)(string + normal_str.size());
		}
	}

	return res;
}

unsigned long wstrtoul(const uint16_t *string, uint16_t **end_ptr, int base) {
	if (!string) {
		if (end_ptr)
			*end_ptr = nullptr;
		return 0;
	}

	std::string normal_str = wideStringToString(string);
	char *normal_end = nullptr;
	unsigned long res = std::strtoul(normal_str.c_str(), &normal_end, base);

	if (end_ptr) {
		if (normal_end && *normal_end) {
			size_t offset = normal_end - normal_str.c_str();
			*end_ptr = (uint16_t *)(string + offset);
		} else {
			*end_ptr = (uint16_t *)(string + normal_str.size());
		}
	}

	return res;
}
