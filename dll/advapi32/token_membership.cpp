#include "token_membership.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "token_identity.h"

#include <array>
#include <cstring>

namespace {

constexpr DWORD kGroupEnabled = 0x00000004;
constexpr DWORD kGroupDenyOnly = 0x00000010;
constexpr DWORD kNoImpersonationToken = 1309;
constexpr DWORD kInvalidSid = 1337;

BOOL membershipFailure(DWORD error) {
	kernel32::setLastError(error);
	return FALSE;
}

bool equalSid(const std::vector<uint8_t> &candidate, const BYTE *sid, size_t size) {
	return candidate.size() == size && std::memcmp(candidate.data(), sid, size) == 0;
}

} // namespace

BOOL WINAPI advapi32::CheckTokenMembership(HANDLE token, PSID sid, LPBOOL member) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CheckTokenMembership(%p, %p, %p)\n", token, sid, member);
	if (!member)
		return membershipFailure(ERROR_INVALID_PARAMETER);
	*member = FALSE;
	std::shared_ptr<wibo::identity::TokenIdentityContext> context;
	if (token) {
		HandleMeta metadata{};
		auto object = wibo::handles().getAs<TokenObject>(token, &metadata);
		if (!object)
			return membershipFailure(ERROR_INVALID_HANDLE);
		if (!(metadata.grantedAccess & TOKEN_QUERY))
			return membershipFailure(ERROR_ACCESS_DENIED);
		if (object->kind != TokenKind::Impersonation)
			return membershipFailure(kNoImpersonationToken);
		context = object->identityContext;
	} else {
		// Token assignment to guest threads is not represented. The captured
		// adapter primary identity supplies the current nonimpersonating scope.
		context = wibo::identity::currentTokenIdentityContext();
	}
	if (!sid)
		return membershipFailure(kInvalidSid);
	std::array<BYTE, 8 + 4 * SID_MAX_SUB_AUTHORITIES> identifier{};
	std::memcpy(identifier.data(), sid, 8);
	if (identifier[0] != SID_REVISION || identifier[1] > SID_MAX_SUB_AUTHORITIES)
		return membershipFailure(kInvalidSid);
	const size_t size = 8 + 4 * size_t(identifier[1]);
	std::memcpy(identifier.data(), sid, size);
	std::shared_ptr<const wibo::identity::TokenSnapshot> snapshot;
	const DWORD error = wibo::identity::tokenSnapshot(context, snapshot);
	if (error != ERROR_SUCCESS)
		return membershipFailure(error);
	if (!snapshot->restrictedSids.empty())
		return membershipFailure(ERROR_NOT_SUPPORTED);
	const bool native = snapshot->membershipDisposition == wibo::identity::MembershipDisposition::NativeEvaluated;
	bool enabled = equalSid(snapshot->user.sid, identifier.data(), size) && (!native || snapshot->nativeMembership[0]);
	for (size_t index = 0; index < snapshot->groups.size(); ++index) {
		const auto &group = snapshot->groups[index];
		const bool groupEnabled = native ? snapshot->nativeMembership[index + 1] != 0
										 : (group.attributes & kGroupEnabled) && !(group.attributes & kGroupDenyOnly);
		if (groupEnabled && equalSid(group.sid, identifier.data(), size)) {
			enabled = true;
			break;
		}
	}
	*member = enabled ? TRUE : FALSE;
	kernel32::setLastError(ERROR_SUCCESS);
	return TRUE;
}
