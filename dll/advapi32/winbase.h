#pragma once

#include "types.h"

enum SID_NAME_USE : DWORD {
	SidTypeUser = 1,
	SidTypeGroup,
	SidTypeDomain,
	SidTypeAlias,
	SidTypeWellKnownGroup,
	SidTypeDeletedAccount,
	SidTypeInvalid,
	SidTypeUnknown,
	SidTypeComputer,
	SidTypeLabel
};

namespace advapi32 {

BOOL WINAPI LookupAccountNameA(LPCSTR system, LPCSTR account, PSID sid, LPDWORD sidSize, LPSTR domain,
							   LPDWORD domainSize, SID_NAME_USE *use);
BOOL WINAPI LookupAccountNameW(LPCWSTR system, LPCWSTR account, PSID sid, LPDWORD sidSize, LPWSTR domain,
							   LPDWORD domainSize, SID_NAME_USE *use);
BOOL WINAPI LookupAccountSidW(LPCWSTR lpSystemName, PSID Sid, LPWSTR Name, LPDWORD cchName, LPWSTR ReferencedDomainName,
							  LPDWORD cchReferencedDomainName, SID_NAME_USE *peUse);
BOOL WINAPI LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
BOOL WINAPI LookupPrivilegeValueW(LPCWSTR lpSystemName, LPCWSTR lpName, PLUID lpLuid);
BOOL WINAPI GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer);
BOOL WINAPI GetUserNameW(LPWSTR lpBuffer, LPDWORD pcbBuffer);

} // namespace advapi32
