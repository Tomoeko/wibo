#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace wibo::identity {

enum class TokenIdentitySource : uint32_t { ProviderProcessPrimary = 1 };
enum class MembershipDisposition : uint32_t { CapturedLists = 1, NativeEvaluated = 2 };

struct SidAndAttributes {
	std::vector<uint8_t> sid;
	uint32_t attributes = 0;
};

struct TokenStatisticsSnapshot {
	uint32_t tokenId[2];
	uint32_t authenticationId[2];
	int64_t expirationTime;
	uint32_t tokenType;
	uint32_t impersonationLevel;
	uint32_t dynamicCharged;
	uint32_t dynamicAvailable;
	uint32_t groupCount;
	uint32_t privilegeCount;
	uint32_t modifiedId[2];
};
static_assert(sizeof(TokenStatisticsSnapshot) == 56);

struct TokenSnapshot {
	TokenIdentitySource source = TokenIdentitySource::ProviderProcessPrimary;
	SidAndAttributes user;
	std::vector<SidAndAttributes> groups;
	std::vector<SidAndAttributes> restrictedSids;
	std::vector<uint8_t> primaryGroup;
	uint32_t elevation = 0;
	MembershipDisposition membershipDisposition = MembershipDisposition::CapturedLists;
	uint32_t restrictionQueryError = 0;
	// Native decisions are ordered as the user followed by each group. They
	// retain membership results without claiming unavailable restriction data.
	std::vector<uint32_t> nativeMembership;
	// These IDs describe the captured provider token, not guest token objects.
	TokenStatisticsSnapshot statistics{};
};

class TokenIdentityContext;

// The optional adapter describes its own process primary token. It does not
// provide credentials for foreign guest processes or guest impersonation.
std::shared_ptr<TokenIdentityContext> currentTokenIdentityContext();
uint32_t tokenSnapshot(const std::shared_ptr<TokenIdentityContext> &context,
					   std::shared_ptr<const TokenSnapshot> &output);

} // namespace wibo::identity
