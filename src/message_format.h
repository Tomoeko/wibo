#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wibo::message {
constexpr uint32_t kMaximumFieldUnits = 4096;

struct Field {
	uint32_t width = 0;
	uint32_t precision = 0;
	bool widthArgument = false;
	bool precisionArgument = false;
	char type = 0;
	std::string_view modifier;

	unsigned stars() const { return unsigned(widthArgument) + unsigned(precisionArgument); }
	bool string() const { return type == 's' || type == 'S'; }
	bool wideString(bool wideApi) const {
		return modifier == "l" || modifier == "w" || (modifier != "h" && (wideApi ? type == 's' : type == 'S'));
	}
};

inline bool parseField(std::string_view specification, Field &field) {
	size_t position = 0;
	while (position < specification.size() &&
		   std::string_view("-+ #0").find(specification[position]) != std::string_view::npos)
		++position;
	const auto decimal = [&](uint32_t &value) {
		while (position < specification.size() && specification[position] >= '0' && specification[position] <= '9') {
			const uint32_t digit = specification[position++] - '0';
			if (value > (kMaximumFieldUnits - digit) / 10)
				return false;
			value = value * 10 + digit;
		}
		return true;
	};
	if (position < specification.size() && specification[position] == '*') {
		field.widthArgument = true;
		++position;
	} else if (!decimal(field.width))
		return false;
	if (position < specification.size() && specification[position] == '.') {
		++position;
		if (position < specification.size() && specification[position] == '*') {
			field.precisionArgument = true;
			++position;
		} else if (!decimal(field.precision))
			return false;
	}
	if (position < specification.size() &&
		std::string_view("hlw").find(specification[position]) != std::string_view::npos)
		field.modifier = specification.substr(position++, 1);
	else if (specification.substr(position, 3) == "I32") {
		field.modifier = specification.substr(position, 3);
		position += 3;
	}
	if (position + 1 != specification.size())
		return false;
	field.type = specification[position];
	return std::string_view("diuoxXcCsS").find(field.type) != std::string_view::npos &&
		   !(field.string() && field.modifier == "I32") &&
		   !(field.modifier == "w" && !field.string() && field.type != 'c' && field.type != 'C');
}

inline bool extent(const Field &field, const uint32_t *arguments, size_t textUnits, size_t &units) {
	unsigned index = 0;
	int64_t width = field.widthArgument ? static_cast<int32_t>(arguments[index++]) : field.width;
	if (width < 0)
		width = -width;
	const int64_t precision =
		field.precisionArgument ? std::max<int64_t>(0, static_cast<int32_t>(arguments[index])) : field.precision;
	if (width > kMaximumFieldUnits || precision > kMaximumFieldUnits)
		return false;
	units = std::max({size_t(width), size_t(precision), field.string() ? textUnits : size_t(32)});
	return true;
}
} // namespace wibo::message
