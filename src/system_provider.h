#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wibo::provider {

// Optional executable protocol: argv contains an operation and UTF-8 arguments;
// stdout is a bounded little-endian response. Stderr remains a diagnostic stream.
// With WIBO_SYSTEM_PROVIDER_PERSISTENT=1, --serve exchanges length-prefixed
// frames over stdin/stdout. Request payloads contain a count and UTF-8 arguments.
constexpr uint32_t kMagic = 0x50535957;
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxResponse = 8 * 1024 * 1024;
constexpr int32_t kUnavailable = static_cast<int32_t>(0x80041001);

bool configured();
std::string encodeBytes(std::string_view input);
bool encodeUtf8(std::u16string_view input, std::string &output);
// A positive WIBO_SYSTEM_PROVIDER_MAX_TIMEOUT_MS caps the caller's lock/IO timeout;
// missing, empty, or invalid settings retain the requested finite timeout.
bool request(const std::vector<std::string> &arguments, std::vector<uint8_t> &response, int timeoutMs = 10000,
			 bool *timedOut = nullptr);

class Reader {
	std::span<const uint8_t> remaining;

  public:
	explicit Reader(std::span<const uint8_t> bytes) : remaining(bytes) {}
	bool number(uint32_t &value);
	bool bytes(std::vector<uint8_t> &value);
	bool text(std::u16string &value);
	bool header(int32_t &status);
	bool done() const { return remaining.empty(); }
};

} // namespace wibo::provider
