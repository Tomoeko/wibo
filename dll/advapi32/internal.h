#pragma once

#include "common.h"
#include "errors.h"
#include "handles.h"
#include "securitybaseapi.h"
#include "token_identity.h"

constexpr DWORD SECURITY_LOCAL_SYSTEM_RID = 18;

constexpr DWORD TOKEN_DUPLICATE = 0x0002;
constexpr DWORD TOKEN_QUERY = 0x0008;

inline DWORD tokenAccessError(DWORD requested, DWORD &granted) {
	constexpr DWORD maximumAllowed = 0x02000000;
	constexpr DWORD supportedAccess = TOKEN_QUERY | TOKEN_DUPLICATE;
	// Only the query and duplication policy of runtime-owned tokens is represented.
	if (requested & ~(maximumAllowed | supportedAccess))
		return ERROR_NOT_SUPPORTED;
	granted = requested & maximumAllowed ? supportedAccess : requested;
	return ERROR_SUCCESS;
}

enum class TokenKind : DWORD { Primary = 1, Impersonation = 2 };

struct TokenObject : ObjectBase {
	static constexpr ObjectType kType = ObjectType::Token;

	Pin<> obj;
	const std::shared_ptr<wibo::identity::TokenIdentityContext> identityContext;
	const TokenKind kind;
	const DWORD impersonationLevel;

	explicit TokenObject(Pin<> obj, std::shared_ptr<wibo::identity::TokenIdentityContext> identityContext,
						 TokenKind kind = TokenKind::Primary, DWORD impersonationLevel = 0)
		: ObjectBase(kType), obj(std::move(obj)), identityContext(std::move(identityContext)), kind(kind),
		  impersonationLevel(impersonationLevel) {}
};

using SidIdentifierAuthority = SID_IDENTIFIER_AUTHORITY;

struct Sid {
	BYTE Revision;
	BYTE SubAuthorityCount;
	SidIdentifierAuthority IdentifierAuthority;
	DWORD SubAuthority[1];
};
