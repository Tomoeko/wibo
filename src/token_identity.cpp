#include "token_identity.h"

#include "errors.h"
#include "system_provider.h"

#include <cstring>
#include <map>
#include <mutex>

namespace wibo::identity {

class TokenIdentityContext {
  public:
	std::mutex mutex;
	std::shared_ptr<const TokenSnapshot> snapshot;
};

namespace {

constexpr uint32_t kInvalidFunction = 1;
constexpr uint32_t kGroupEnabled = 4;
constexpr uint32_t kGroupDenyOnly = 16;

bool validSid(const std::vector<uint8_t> &sid) {
	return sid.size() >= 8 && sid[0] == 1 && sid[1] <= 15 && sid.size() == 8 + 4 * size_t(sid[1]);
}

bool readSid(provider::Reader &reader, std::vector<uint8_t> &sid) { return reader.bytes(sid) && validSid(sid); }

bool readGroups(provider::Reader &reader, std::vector<SidAndAttributes> &groups) {
	uint32_t count = 0;
	// Each entry needs an attribute DWORD, a length DWORD, and at least 8 SID bytes.
	if (!reader.number(count) || count > provider::kMaxResponse / 16)
		return false;
	for (uint32_t index = 0; index < count; ++index) {
		SidAndAttributes group;
		if (!reader.number(group.attributes) || !readSid(reader, group.sid))
			return false;
		groups.push_back(std::move(group));
	}
	return true;
}

bool readMembership(provider::Reader &reader, TokenSnapshot &snapshot) {
	uint32_t disposition = 0, count = 0;
	if (!reader.number(disposition) || !reader.number(snapshot.restrictionQueryError) || !reader.number(count))
		return false;
	if (disposition == static_cast<uint32_t>(MembershipDisposition::CapturedLists)) {
		if (snapshot.restrictionQueryError || count)
			return false;
		snapshot.membershipDisposition = MembershipDisposition::CapturedLists;
		return true;
	}
	if (disposition != static_cast<uint32_t>(MembershipDisposition::NativeEvaluated) ||
		(snapshot.restrictionQueryError != kInvalidFunction && snapshot.restrictionQueryError != ERROR_NOT_SUPPORTED &&
		 snapshot.restrictionQueryError != ERROR_INVALID_PARAMETER) ||
		!snapshot.restrictedSids.empty() || count != snapshot.groups.size() + 1)
		return false;
	snapshot.membershipDisposition = MembershipDisposition::NativeEvaluated;
	std::map<std::vector<uint8_t>, uint32_t> decisions;
	for (uint32_t index = 0; index < count; ++index) {
		uint32_t member = 0;
		if (!reader.number(member) || member > 1)
			return false;
		const auto &entry = index ? snapshot.groups[index - 1] : snapshot.user;
		if (index && member && (!(entry.attributes & kGroupEnabled) || (entry.attributes & kGroupDenyOnly)))
			return false;
		const auto [previous, inserted] = decisions.emplace(entry.sid, member);
		if (!inserted && previous->second != member)
			return false;
		snapshot.nativeMembership.push_back(member);
	}
	return true;
}

uint32_t captureTokenSnapshot(std::shared_ptr<const TokenSnapshot> &output) {
	std::vector<uint8_t> response;
	if (!provider::request({"token-identity"}, response))
		return ERROR_NOT_SUPPORTED;
	provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return ERROR_INVALID_DATA;
	if (status == provider::kUnavailable)
		return reader.done() ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA;
	if (status != ERROR_SUCCESS)
		return reader.done() && status > 0 ? static_cast<uint32_t>(status) : ERROR_INVALID_DATA;
	TokenSnapshot snapshot;
	uint32_t source = 0;
	std::vector<uint8_t> statistics;
	if (!reader.number(source) || source != static_cast<uint32_t>(TokenIdentitySource::ProviderProcessPrimary) ||
		!reader.bytes(statistics) || statistics.size() != sizeof(snapshot.statistics) ||
		!reader.number(snapshot.user.attributes) || !readSid(reader, snapshot.user.sid) ||
		!readGroups(reader, snapshot.groups) || !readGroups(reader, snapshot.restrictedSids) ||
		!readSid(reader, snapshot.primaryGroup) || !reader.number(snapshot.elevation) || snapshot.elevation > 1 ||
		!readMembership(reader, snapshot) || !reader.done())
		return ERROR_INVALID_DATA;
	std::memcpy(&snapshot.statistics, statistics.data(), statistics.size());
	if (snapshot.statistics.tokenType != 1 || snapshot.statistics.groupCount != snapshot.groups.size())
		return ERROR_INVALID_DATA;
	// User attributes are reserved for the ordinary primary-token scope.
	if (snapshot.user.attributes)
		return ERROR_NOT_SUPPORTED;
	output = std::make_shared<const TokenSnapshot>(std::move(snapshot));
	return ERROR_SUCCESS;
}

} // namespace

std::shared_ptr<TokenIdentityContext> currentTokenIdentityContext() {
	static const auto context = std::make_shared<TokenIdentityContext>();
	return context;
}

uint32_t tokenSnapshot(const std::shared_ptr<TokenIdentityContext> &context,
					   std::shared_ptr<const TokenSnapshot> &output) {
	if (!context)
		return ERROR_NOT_SUPPORTED;
	std::lock_guard lock(context->mutex);
	if (!context->snapshot) {
		const uint32_t status = captureTokenSnapshot(context->snapshot);
		if (status != ERROR_SUCCESS)
			return status;
	}
	output = context->snapshot;
	return ERROR_SUCCESS;
}

} // namespace wibo::identity
