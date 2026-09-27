#include "securitybaseapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"
#include "security_descriptor.h"
#include "strutil.h"
#include "system_provider.h"
#include "types.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

constexpr size_t kAceAlignment = 4;
constexpr DWORD ERROR_REVISION_MISMATCH = 1306;
constexpr DWORD ERROR_INVALID_ACL = 1336;
constexpr DWORD ERROR_INVALID_SID = 1337;
constexpr DWORD ERROR_ALLOTTED_SPACE_EXCEEDED = 1344;
constexpr DWORD ERROR_INVALID_SECURITY_DESCR = 1338;

struct SidAndAttributes {
	GUEST_PTR SidPtr;
	DWORD Attributes;
};

struct TokenUserData {
	SidAndAttributes User;
};

struct TokenStatisticsData {
	LUID tokenId{};
	LUID authenticationId{};
	LARGE_INTEGER expirationTime{};
	DWORD tokenType = 0;
	DWORD impersonationLevel = 0;
	DWORD dynamicCharged = 0;
	DWORD dynamicAvailable = 0;
	DWORD groupCount = 0;
	DWORD privilegeCount = 0;
	LUID modifiedId{};
};

struct TokenPrimaryGroupData {
	GUEST_PTR PrimaryGroup;
};
static_assert(sizeof(TokenUserData) == (sizeof(GUEST_PTR) == 8 ? 16 : 8));
static_assert(sizeof(TokenPrimaryGroupData) == sizeof(GUEST_PTR));
static_assert(sizeof(TokenStatisticsData) == 56);

size_t alignToDword(size_t value) { return (value + (kAceAlignment - 1)) & ~(kAceAlignment - 1); }

size_t sidLength(const Sid *sid) {
	if (!sid) {
		return 0;
	}
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		return 0;
	}
	size_t base = sizeof(Sid) - sizeof(DWORD);
	size_t extra = static_cast<size_t>(sid->SubAuthorityCount) * sizeof(DWORD);
	return base + extra;
}

bool computeAclUsedSize(const ACL *acl, size_t capacity, size_t &used) {
	if (!acl || capacity < sizeof(ACL)) {
		return false;
	}
	size_t offset = sizeof(ACL);
	const BYTE *base = reinterpret_cast<const BYTE *>(acl);
	for (WORD i = 0; i < acl->AceCount; ++i) {
		if (offset + sizeof(ACE_HEADER) > capacity) {
			return false;
		}
		const auto *header = reinterpret_cast<const ACE_HEADER *>(base + offset);
		if (header->AceSize < sizeof(ACE_HEADER) || header->AceSize % 4) {
			return false;
		}
		size_t aceSize = header->AceSize;
		if (offset + aceSize > capacity) {
			return false;
		}
		offset += aceSize;
	}
	used = offset;
	return true;
}

BOOL providerFileSecurity(const std::vector<std::string> &arguments, PSECURITY_DESCRIPTOR descriptor, DWORD length,
						  LPDWORD needed) {
	if (!needed) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request(arguments, response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	if (status != ERROR_SUCCESS) {
		kernel32::setLastError(reader.done() ? status : ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	std::vector<uint8_t> data;
	if (!reader.bytes(data) || !reader.done() || !wibo::security::validRelativeDescriptor(data)) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	*needed = static_cast<DWORD>(data.size());
	if (!descriptor || length < data.size()) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	std::memcpy(descriptor, data.data(), data.size());
	return TRUE;
}

bool serializeDescriptor(PSECURITY_DESCRIPTOR source, DWORD information, std::vector<uint8_t> &data) {
	if (!source || source->Revision != SECURITY_DESCRIPTOR_REVISION)
		return false;
	SECURITY_DESCRIPTOR_RELATIVE wire{};
	wire.Revision = source->Revision;
	wire.Sbz1 = source->Sbz1;
	wire.Control = source->Control | SE_SELF_RELATIVE;
	data.assign(sizeof(wire), 0);
	const auto pointer = [source](GUEST_PTR absolute, DWORD offset) -> const uint8_t * {
		if (source->Control & SE_SELF_RELATIVE) {
			if (!offset)
				return nullptr;
			if (offset < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || offset % 4 || offset >= wibo::provider::kMaxResponse)
				return nullptr;
			return reinterpret_cast<const uint8_t *>(source) + offset;
		}
		return fromGuestPtr<uint8_t>(absolute);
	};
	const auto *relative = reinterpret_cast<const SECURITY_DESCRIPTOR_RELATIVE *>(source);
	const bool isRelative = source->Control & SE_SELF_RELATIVE;
	const auto append = [&data](const uint8_t *bytes, size_t size, DWORD &offset) {
		if (!bytes) {
			offset = 0;
			return true;
		}
		const size_t padding = (4 - data.size() % 4) % 4;
		if (padding > wibo::provider::kMaxResponse - data.size() ||
			size > wibo::provider::kMaxResponse - data.size() - padding)
			return false;
		data.insert(data.end(), padding, 0);
		offset = static_cast<DWORD>(data.size());
		data.insert(data.end(), bytes, bytes + size);
		return true;
	};
	const auto sid = [&](GUEST_PTR absolute, DWORD originalOffset, DWORD &offset) {
		const uint8_t *bytes = pointer(absolute, originalOffset);
		if (!bytes && (isRelative ? originalOffset != 0 : absolute != 0))
			return false;
		if (bytes && (bytes[0] != SID_REVISION || bytes[1] > SID_MAX_SUB_AUTHORITIES))
			return false;
		return append(bytes, bytes ? 8 + 4 * size_t(bytes[1]) : 0, offset);
	};
	const auto acl = [&](GUEST_PTR absolute, DWORD originalOffset, DWORD &offset) {
		const uint8_t *bytes = pointer(absolute, originalOffset);
		if (!bytes && (isRelative ? originalOffset != 0 : absolute != 0))
			return false;
		if (!bytes)
			return append(nullptr, 0, offset);
		const auto *list = reinterpret_cast<const ACL *>(bytes);
		size_t used = 0;
		return computeAclUsedSize(list, list->AclSize, used) && append(bytes, list->AclSize, offset);
	};
	// Do not read unrequested pointers from a caller's absolute descriptor.
	if ((information & 1) && !sid(isRelative ? 0 : source->Owner, isRelative ? relative->Owner : 0, wire.Owner))
		return false;
	if ((information & 2) && !sid(isRelative ? 0 : source->Group, isRelative ? relative->Group : 0, wire.Group))
		return false;
	if ((information & 4) && (source->Control & SE_DACL_PRESENT) &&
		!acl(isRelative ? 0 : source->Dacl, isRelative ? relative->Dacl : 0, wire.Dacl))
		return false;
	if ((information & 8) && (source->Control & 0x10) &&
		!acl(isRelative ? 0 : source->Sacl, isRelative ? relative->Sacl : 0, wire.Sacl))
		return false;
	std::memcpy(data.data(), &wire, sizeof(wire));
	return wibo::security::validRelativeDescriptor(data);
}

BOOL providerSetFileSecurity(const std::vector<std::string> &arguments, DWORD information,
							 PSECURITY_DESCRIPTOR descriptor) {
	// Only owner, group and traditional ACL components are serialized here.
	constexpr DWORD supportedInformation = 0xF000000F;
	if (information & ~supportedInformation) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::vector<uint8_t> data;
	if (!serializeDescriptor(descriptor, information, data)) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	std::vector<std::string> request = arguments;
	request.push_back(std::to_string(information));
	request.push_back(
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(data.data()), data.size())));
	size_t commandLength = 0;
	for (const auto &argument : request)
		commandLength += argument.size() + 3;
	// The adapter's Windows command line is limited to 32767 UTF-16 code units.
	// UTF-8 byte counts are a conservative bound; reserve room for its image path.
	if (commandLength > 28000) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request(request, response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status) || !reader.done()) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	if (status != ERROR_SUCCESS) {
		kernel32::setLastError(status);
		return FALSE;
	}
	return TRUE;
}

} // namespace

namespace advapi32 {

BOOL WINAPI SetFileSecurityA(LPCSTR path, SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetFileSecurityA(%p, 0x%x, %p)\n", path, information, descriptor);
	if (!path) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return providerSetFileSecurity({"set-file-security-a", wibo::provider::encodeBytes(path)}, information, descriptor);
}

BOOL WINAPI SetFileSecurityW(LPCWSTR path, SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetFileSecurityW(%p, 0x%x, %p)\n", path, information, descriptor);
	std::string encoded;
	if (!path || !wibo::provider::encodeUtf8(
					 std::u16string_view(reinterpret_cast<const char16_t *>(path), wstrlen(path)), encoded)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return providerSetFileSecurity({"set-file-security-w", encoded}, information, descriptor);
}

BOOL WINAPI GetFileSecurityA(LPCSTR path, SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor,
							 DWORD length, LPDWORD needed) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileSecurityA(%p, 0x%x, %p, %u, %p)\n", path, information, descriptor, length, needed);
	if (!path) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return providerFileSecurity({"file-security-a", wibo::provider::encodeBytes(path), std::to_string(information)},
								descriptor, length, needed);
}

BOOL WINAPI GetFileSecurityW(LPCWSTR path, SECURITY_INFORMATION information, PSECURITY_DESCRIPTOR descriptor,
							 DWORD length, LPDWORD needed) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileSecurityW(%p, 0x%x, %p, %u, %p)\n", path, information, descriptor, length, needed);
	std::string encoded;
	if (!path || !wibo::provider::encodeUtf8(
					 std::u16string_view(reinterpret_cast<const char16_t *>(path), wstrlen(path)), encoded)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return providerFileSecurity({"file-security-w", encoded, std::to_string(information)}, descriptor, length, needed);
}

BOOL WINAPI AddAce(PACL acl, DWORD revision, DWORD index, LPVOID list, DWORD length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AddAce(%p, %u, %u, %p, %u)\n", acl, revision, index, list, length);
	if (!acl || (!list && length) || revision < ACL_REVISION2 || revision > ACL_REVISION4 ||
		(index > acl->AceCount && index != std::numeric_limits<DWORD>::max())) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t used = 0;
	if (!computeAclUsedSize(acl, acl->AclSize, used)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	if (length > acl->AclSize - used) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	const auto *bytes = static_cast<const BYTE *>(list);
	size_t offset = 0;
	DWORD count = 0;
	while (offset < length) {
		if (length - offset < sizeof(ACE_HEADER)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		ACE_HEADER header{};
		std::memcpy(&header, bytes + offset, sizeof(header));
		if (header.AceSize < sizeof(header) || header.AceSize % 4 || header.AceSize > length - offset ||
			(header.AceType >= 5 && header.AceType <= 8 && revision != ACL_REVISION4)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		offset += header.AceSize;
		++count;
	}
	if (count > std::numeric_limits<WORD>::max() - acl->AceCount) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (!length)
		return TRUE;
	std::vector<BYTE> copy(bytes, bytes + length);
	auto *base = reinterpret_cast<BYTE *>(acl);
	size_t insertion = sizeof(ACL);
	for (DWORD i = 0; i < std::min<DWORD>(index, acl->AceCount); ++i)
		insertion += reinterpret_cast<const ACE_HEADER *>(base + insertion)->AceSize;
	std::memmove(base + insertion + length, base + insertion, used - insertion);
	std::memcpy(base + insertion, copy.data(), copy.size());
	acl->AceCount = static_cast<WORD>(acl->AceCount + count);
	acl->AclRevision = std::max<BYTE>(acl->AclRevision, static_cast<BYTE>(revision));
	return TRUE;
}

BOOL WINAPI GetAce(PACL acl, DWORD index, GUEST_PTR *ace) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAce(%p, %u, %p)\n", acl, index, ace);
	if (!acl || !ace || index >= acl->AceCount) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t used = 0;
	if (!computeAclUsedSize(acl, acl->AclSize, used)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	const auto *cursor = reinterpret_cast<const BYTE *>(acl) + sizeof(ACL);
	for (DWORD i = 0; i < index; ++i)
		cursor += reinterpret_cast<const ACE_HEADER *>(cursor)->AceSize;
	*ace = toGuestPtr(cursor);
	return TRUE;
}

DWORD WINAPI GetLengthSid(PSID sid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLengthSid(%p)\n", sid);
	const size_t length = sidLength(reinterpret_cast<const Sid *>(sid));
	return static_cast<DWORD>(length);
}

BOOL WINAPI GetAclInformation(PACL acl, LPVOID information, DWORD length, DWORD informationClass) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAclInformation(%p, %p, %u, %u)\n", acl, information, length, informationClass);
	const DWORD needed = informationClass == 1 ? sizeof(DWORD) : informationClass == 2 ? 3 * sizeof(DWORD) : 0;
	if (!acl || !information || !needed || length < needed) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t used = 0;
	if (!computeAclUsedSize(acl, acl->AclSize, used)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	if (informationClass == 1) {
		const DWORD revision = acl->AclRevision;
		std::memcpy(information, &revision, sizeof(revision));
	} else {
		const DWORD values[] = {acl->AceCount, static_cast<DWORD>(used), static_cast<DWORD>(acl->AclSize - used)};
		std::memcpy(information, values, sizeof(values));
	}
	return TRUE;
}

BOOL WINAPI InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeAcl(%p, %u, %u)\n", pAcl, nAclLength, dwAclRevision);
	if (!pAcl) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (nAclLength < sizeof(ACL) || nAclLength > std::numeric_limits<WORD>::max()) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BYTE revision = static_cast<BYTE>(dwAclRevision);
	switch (revision) {
	case ACL_REVISION1:
	case ACL_REVISION2:
	case ACL_REVISION3:
	case ACL_REVISION4:
		break;
	default:
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pAcl->AclRevision = revision;
	pAcl->Sbz1 = 0;
	pAcl->AclSize = static_cast<WORD>(nAclLength);
	pAcl->AceCount = 0;
	pAcl->Sbz2 = 0;
	return TRUE;
}

BOOL WINAPI AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AddAccessAllowedAce(%p, %u, 0x%x, %p)\n", pAcl, dwAceRevision, AccessMask, pSid);
	if (!pAcl || !pSid) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BYTE revision = static_cast<BYTE>(dwAceRevision);
	switch (revision) {
	case ACL_REVISION1:
	case ACL_REVISION2:
	case ACL_REVISION3:
	case ACL_REVISION4:
		break;
	default:
		kernel32::setLastError(ERROR_REVISION_MISMATCH);
		return FALSE;
	}
	if (pAcl->AclRevision < revision) {
		kernel32::setLastError(ERROR_REVISION_MISMATCH);
		return FALSE;
	}
	if (pAcl->AceCount == std::numeric_limits<WORD>::max()) {
		kernel32::setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	size_t capacity = pAcl->AclSize;
	if (capacity < sizeof(ACL)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	size_t used = 0;
	if (!computeAclUsedSize(pAcl, capacity, used)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	const auto *sid = reinterpret_cast<const Sid *>(pSid);
	size_t sidLen = sidLength(sid);
	if (sidLen == 0 || sidLen > capacity) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	size_t aceSize = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sidLen;
	aceSize = alignToDword(aceSize);
	if (aceSize > std::numeric_limits<WORD>::max()) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	if (used + aceSize > capacity) {
		kernel32::setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	auto *dest = reinterpret_cast<BYTE *>(pAcl) + used;
	std::memset(dest, 0, aceSize);
	auto *ace = reinterpret_cast<ACCESS_ALLOWED_ACE *>(dest);
	ace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
	ace->Header.AceFlags = 0;
	ace->Header.AceSize = static_cast<WORD>(aceSize);
	ace->Mask = AccessMask;
	std::memcpy(&ace->SidStart, sid, sidLen);
	pAcl->AceCount = static_cast<WORD>(pAcl->AceCount + 1);
	return TRUE;
}

BOOL WINAPI FindFirstFreeAce(PACL pAcl, GUEST_PTR *pAce) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFreeAce(%p, %p)\n", pAcl, pAce);
	if (!pAce) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*pAce = GUEST_NULL;
	if (!pAcl) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t capacity = pAcl->AclSize;
	size_t used = 0;
	if (!computeAclUsedSize(pAcl, capacity, used)) {
		kernel32::setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	*pAce = used < capacity ? toGuestPtr(reinterpret_cast<BYTE *>(pAcl) + used) : GUEST_NULL;
	return TRUE;
}

BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, LPBOOL lpbDaclPresent, GUEST_PTR *pDacl,
									  LPBOOL lpbDaclDefaulted) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSecurityDescriptorDacl(%p, %p, %p, %p)\n", pSecurityDescriptor, lpbDaclPresent, pDacl,
			  lpbDaclDefaulted);
	if (!pSecurityDescriptor) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	if (!lpbDaclPresent) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (pSecurityDescriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	BOOL hasDacl = (pSecurityDescriptor->Control & SE_DACL_PRESENT) ? TRUE : FALSE;
	*lpbDaclPresent = hasDacl;
	if (!hasDacl)
		return TRUE;
	if (pDacl) {
		if (pSecurityDescriptor->Control & SE_SELF_RELATIVE) {
			const auto *relative = reinterpret_cast<const SECURITY_DESCRIPTOR_RELATIVE *>(pSecurityDescriptor);
			*pDacl =
				relative->Dacl ? toGuestPtr(reinterpret_cast<const BYTE *>(relative) + relative->Dacl) : GUEST_NULL;
		} else {
			*pDacl = pSecurityDescriptor->Dacl;
		}
	}
	if (lpbDaclDefaulted) {
		*lpbDaclDefaulted = (pSecurityDescriptor->Control & SE_DACL_DEFAULTED) ? TRUE : FALSE;
	}
	return TRUE;
}

PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidIdentifierAuthority(%p)\n", pSid);
	if (!pSid) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	auto *sid = reinterpret_cast<Sid *>(pSid);
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	return reinterpret_cast<PSID_IDENTIFIER_AUTHORITY>(&sid->IdentifierAuthority);
}

PUCHAR WINAPI GetSidSubAuthorityCount(PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidSubAuthorityCount(%p)\n", pSid);
	if (!pSid) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	auto *sid = reinterpret_cast<Sid *>(pSid);
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	return &sid->SubAuthorityCount;
}

PDWORD WINAPI GetSidSubAuthority(PSID pSid, DWORD nSubAuthority) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidSubAuthority(%p, %u)\n", pSid, nSubAuthority);
	if (!pSid) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	auto *sid = reinterpret_cast<Sid *>(pSid);
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES || nSubAuthority >= sid->SubAuthorityCount) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return nullptr;
	}
	return &sid->SubAuthority[nSubAuthority];
}

BOOL WINAPI ImpersonateLoggedOnUser(HANDLE hToken) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ImpersonateLoggedOnUser(%p)\n", hToken);
	const auto token = wibo::handles().getAs<TokenObject>(hToken);
	// Thread-token assignment is unavailable; do not claim to replace the active identity.
	kernel32::setLastError(token ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
	return FALSE;
}

BOOL WINAPI DuplicateTokenEx(HANDLE hExistingToken, DWORD dwDesiredAccess, void *lpTokenAttributes,
							 DWORD ImpersonationLevel, DWORD TokenType, PHANDLE phNewToken) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DuplicateTokenEx(%p, 0x%x, %p, %u, %u, %p)\n", hExistingToken, dwDesiredAccess, lpTokenAttributes,
			  ImpersonationLevel, TokenType, phNewToken);
	if (!phNewToken) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*phNewToken = NO_HANDLE;
	HandleMeta metadata{};
	auto existing = wibo::handles().getAs<TokenObject>(hExistingToken, &metadata);
	if (!existing) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & TOKEN_DUPLICATE)) {
		kernel32::setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (TokenType != static_cast<DWORD>(TokenKind::Primary) &&
		TokenType != static_cast<DWORD>(TokenKind::Impersonation)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const auto kind = static_cast<TokenKind>(TokenType);
	if (kind == TokenKind::Impersonation &&
		(ImpersonationLevel > 3 ||
		 (existing->kind == TokenKind::Impersonation && ImpersonationLevel > existing->impersonationLevel))) {
		kernel32::setLastError(1346); // ERROR_BAD_IMPERSONATION_LEVEL
		return FALSE;
	}
	const auto *attributes = static_cast<const SECURITY_ATTRIBUTES *>(lpTokenAttributes);
	if (attributes && attributes->lpSecurityDescriptor != GUEST_NULL) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	DWORD granted = metadata.grantedAccess;
	DWORD error = dwDesiredAccess ? tokenAccessError(dwDesiredAccess, granted) : ERROR_SUCCESS;
	if (error != ERROR_SUCCESS) {
		kernel32::setLastError(error);
		return FALSE;
	}
	auto newToken = make_pin<TokenObject>(existing->obj.clone(), existing->identityContext, kind,
										  kind == TokenKind::Impersonation ? ImpersonationLevel : 0);
	const DWORD flags = attributes && attributes->bInheritHandle ? HANDLE_FLAG_INHERIT : 0;
	*phNewToken = wibo::handles().alloc(std::move(newToken), granted, flags);
	return TRUE;
}

BOOL WINAPI CopySid(DWORD nDestinationSidLength, PSID pDestinationSid, PSID pSourceSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CopySid(%u, %p, %p)\n", nDestinationSidLength, pDestinationSid, pSourceSid);
	if (!pDestinationSid || !pSourceSid) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto *source = reinterpret_cast<Sid *>(pSourceSid);
	size_t required = sidLength(source);
	if (required == 0 || required > nDestinationSidLength) {
		kernel32::setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	std::memcpy(pDestinationSid, pSourceSid, required);
	return TRUE;
}

BOOL WINAPI InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSid(%p, %p, %u)\n", sid, pIdentifierAuthority, nSubAuthorityCount);
	if (!sid || !pIdentifierAuthority) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (nSubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	auto *sidStruct = reinterpret_cast<Sid *>(sid);
	sidStruct->Revision = SID_REVISION;
	sidStruct->SubAuthorityCount = nSubAuthorityCount;
	sidStruct->IdentifierAuthority = *pIdentifierAuthority;
	if (nSubAuthorityCount > 0) {
		std::memset(sidStruct->SubAuthority, 0, sizeof(DWORD) * nSubAuthorityCount);
	}
	return TRUE;
}

BOOL WINAPI EqualSid(PSID pSid1, PSID pSid2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EqualSid(%p, %p)\n", pSid1, pSid2);
	if (!pSid1 || !pSid2) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	const auto *sid1 = reinterpret_cast<const Sid *>(pSid1);
	const auto *sid2 = reinterpret_cast<const Sid *>(pSid2);
	if (sid1->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES || sid2->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32::setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	bool equal =
		sid1->Revision == sid2->Revision &&
		std::memcmp(&sid1->IdentifierAuthority, &sid2->IdentifierAuthority, sizeof(SidIdentifierAuthority)) == 0 &&
		sid1->SubAuthorityCount == sid2->SubAuthorityCount;
	if (equal && sid1->SubAuthorityCount > 0) {
		equal = std::memcmp(sid1->SubAuthority, sid2->SubAuthority, sizeof(DWORD) * sid1->SubAuthorityCount) == 0;
	}
	return equal ? TRUE : FALSE;
}

BOOL WINAPI SetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
									PSECURITY_DESCRIPTOR SecurityDescriptor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetKernelObjectSecurity(%p, 0x%x, %p)\n", Handle, SecurityInformation, SecurityDescriptor);
	(void)SecurityInformation;
	if (!SecurityDescriptor) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	auto obj = wibo::handles().get(Handle);
	if (!obj) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR descriptor, WORD *control, DWORD *revision) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSecurityDescriptorControl(%p, %p, %p)\n", descriptor, control, revision);
	if (!descriptor || !control || !revision) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*revision = descriptor->Revision;
	if (descriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32::setLastError(1305); // ERROR_UNKNOWN_REVISION
		return FALSE;
	}
	*control = descriptor->Control;
	return TRUE;
}

BOOL WINAPI SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR descriptor, WORD interest, WORD values) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetSecurityDescriptorControl(%p, 0x%x, 0x%x)\n", descriptor, interest, values);
	if (!descriptor || (interest & ~0x3F00)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (descriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	descriptor->Control = static_cast<WORD>((descriptor->Control & ~interest) | (values & interest));
	return TRUE;
}

BOOL WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD dwRevision) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSecurityDescriptor(%p, %u)\n", pSecurityDescriptor, dwRevision);
	if (!pSecurityDescriptor || dwRevision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pSecurityDescriptor->Revision = static_cast<BYTE>(dwRevision);
	pSecurityDescriptor->Sbz1 = 0;
	pSecurityDescriptor->Control = 0;
	pSecurityDescriptor->Owner = GUEST_NULL;
	pSecurityDescriptor->Group = GUEST_NULL;
	pSecurityDescriptor->Sacl = GUEST_NULL;
	pSecurityDescriptor->Dacl = GUEST_NULL;
	return TRUE;
}

BOOL WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL bDaclPresent, PACL pDacl,
									  BOOL bDaclDefaulted) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetSecurityDescriptorDacl(%p, %u, %p, %u)\n", pSecurityDescriptor, bDaclPresent, pDacl, bDaclDefaulted);
	if (!pSecurityDescriptor) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (pSecurityDescriptor->Revision != SECURITY_DESCRIPTOR_REVISION ||
		(pSecurityDescriptor->Control & SE_SELF_RELATIVE)) {
		kernel32::setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	WORD control = static_cast<WORD>(pSecurityDescriptor->Control & ~(SE_DACL_PRESENT | SE_DACL_DEFAULTED));
	if (bDaclPresent) {
		control = static_cast<WORD>(control | SE_DACL_PRESENT);
		if (bDaclDefaulted) {
			control = static_cast<WORD>(control | SE_DACL_DEFAULTED);
		}
		pSecurityDescriptor->Dacl = toGuestPtr(pDacl);
	} else {
		pSecurityDescriptor->Dacl = GUEST_NULL;
	}
	pSecurityDescriptor->Control = control;
	return TRUE;
}

BOOL WINAPI GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength, LPDWORD ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTokenInformation(%p, %u, %p, %u, %p)\n", TokenHandle, TokenInformationClass, TokenInformation,
			  TokenInformationLength, ReturnLength);
	if (!ReturnLength) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const bool scalar = TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenType ||
						TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenImpersonationLevel;
	const DWORD fixedLength = scalar || TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenElevation ? sizeof(DWORD)
							  : TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenStatistics
								  ? sizeof(TokenStatisticsData)
								  : 0;
	if (fixedLength) {
		*ReturnLength = fixedLength;
		if (TokenInformationLength < fixedLength) {
			kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
	}
	HandleMeta metadata{};
	auto token = wibo::handles().getAs<TokenObject>(TokenHandle, &metadata);
	if (!token) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!(metadata.grantedAccess & TOKEN_QUERY)) {
		kernel32::setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (scalar) {
		if (!TokenInformation || (TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenImpersonationLevel &&
								  token->kind == TokenKind::Primary)) {
			kernel32::setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		const DWORD value = TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenType
								? static_cast<DWORD>(token->kind)
								: token->impersonationLevel;
		std::memcpy(TokenInformation, &value, sizeof(value));
		return TRUE;
	}
	if (TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenStatistics) {
		// The adapter's IDs belong to its captured token, not these guest token objects.
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!fixedLength)
		*ReturnLength = 0;
	if (TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenUser ||
		TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenPrimaryGroup ||
		TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenElevation) {
		const DWORD incomingError = kernel32::getLastError();
		std::shared_ptr<const wibo::identity::TokenSnapshot> snapshot;
		const DWORD error = wibo::identity::tokenSnapshot(token->identityContext, snapshot);
		if (error != ERROR_SUCCESS) {
			kernel32::setLastError(error);
			return FALSE;
		}
		const bool user = TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenUser;
		const bool group = TokenInformationClass == TOKEN_INFORMATION_CLASS::TokenPrimaryGroup;
		const auto &sid = user ? snapshot->user.sid : snapshot->primaryGroup;
		const size_t headerSize = user ? sizeof(TokenUserData) : group ? sizeof(TokenPrimaryGroupData) : 0;
		const DWORD required = static_cast<DWORD>(headerSize ? headerSize + sid.size() : sizeof(DWORD));
		*ReturnLength = required;
		if (!TokenInformation || TokenInformationLength < required) {
			kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		auto *bytes = static_cast<BYTE *>(TokenInformation);
		if (headerSize) {
			std::memcpy(bytes + headerSize, sid.data(), sid.size());
			const GUEST_PTR pointer = toGuestPtr(bytes + headerSize);
			std::memcpy(bytes, &pointer, sizeof(pointer));
			if (user)
				std::memcpy(bytes + offsetof(SidAndAttributes, Attributes), &snapshot->user.attributes, sizeof(DWORD));
		} else {
			std::memcpy(bytes, &snapshot->elevation, sizeof(DWORD));
		}
		kernel32::setLastError(incomingError);
		return TRUE;
	}
	kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

BOOL WINAPI AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
								  DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, LPDWORD ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AdjustTokenPrivileges(%p, %u, %p, %u, %p, %p)\n", TokenHandle, DisableAllPrivileges, NewState,
			  BufferLength, PreviousState, ReturnLength);
	(void)TokenHandle;
	(void)DisableAllPrivileges;
	(void)NewState;
	(void)BufferLength;
	(void)PreviousState;
	(void)ReturnLength;
	return TRUE;
}

BOOL WINAPI SetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetTokenInformation(%p, %u, %p, %u)\n", TokenHandle, TokenInformationClass, TokenInformation,
			  TokenInformationLength);
	(void)TokenInformationClass;
	(void)TokenInformation;
	(void)TokenInformationLength;
	auto token = wibo::handles().getAs<TokenObject>(TokenHandle);
	if (!token) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

} // namespace advapi32
