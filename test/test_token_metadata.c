#include <windows.h>

#include "test_assert.h"

static const DWORD seed = 0x13579bdf;

static void check_scalar(HANDLE token, TOKEN_INFORMATION_CLASS kind, DWORD length, DWORD error, DWORD value) {
	struct {
		DWORD before;
		DWORD value;
		DWORD after[2];
	} buffer;
	memset(&buffer, 0xa5, sizeof(buffer));
	DWORD needed = seed;
	SetLastError(seed);
	BOOL result = GetTokenInformation(token, kind, length ? &buffer.value : NULL, length, &needed);
	TEST_CHECK_EQ(4, needed);
	TEST_CHECK_EQ(error == seed, !!result);
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_EQ(error == seed ? value : 0xa5a5a5a5, buffer.value);
	TEST_CHECK_EQ(0xa5a5a5a5, buffer.before);
	TEST_CHECK_EQ(0xa5a5a5a5, buffer.after[0]);
	TEST_CHECK_EQ(0xa5a5a5a5, buffer.after[1]);
}

static void check_queries(HANDLE token, TOKEN_TYPE type, SECURITY_IMPERSONATION_LEVEL level, DWORD accessError) {
	const DWORD lengths[] = {0, 1, 3, 4, 5, 8};
	for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		DWORD length = lengths[i];
		check_scalar(token, TokenType, length, length < 4 ? ERROR_INSUFFICIENT_BUFFER : accessError, type);
		DWORD levelError = accessError == seed && type == TokenPrimary ? ERROR_INVALID_PARAMETER : accessError;
		check_scalar(token, TokenImpersonationLevel, length, length < 4 ? ERROR_INSUFFICIENT_BUFFER : levelError,
					 level);
	}
}

static HANDLE duplicate(HANDLE token, DWORD access, SECURITY_IMPERSONATION_LEVEL level, TOKEN_TYPE type,
						DWORD expectedError, BOOL inherit) {
	HANDLE output = (HANDLE)(ULONG_PTR)0x12345678;
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, inherit};
	SetLastError(seed);
	BOOL result = DuplicateTokenEx(token, access, inherit ? &attributes : NULL, level, type, &output);
	TEST_CHECK_EQ(expectedError == seed, !!result);
	TEST_CHECK_EQ(expectedError, GetLastError());
	if (result) {
		TEST_CHECK(output != NULL && output != INVALID_HANDLE_VALUE);
		DWORD flags = seed;
		TEST_CHECK(GetHandleInformation(output, &flags));
		TEST_CHECK_EQ(inherit ? HANDLE_FLAG_INHERIT : 0, flags);
	} else {
		TEST_CHECK(output == NULL);
	}
	return output;
}

static void check_gap_cases(HANDLE base) {
	SetLastError(seed);
	TEST_CHECK(!ImpersonateLoggedOnUser(base));
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	check_queries(base, TokenPrimary, SecurityAnonymous, seed);
	HANDLE threadToken = (HANDLE)(ULONG_PTR)0x12345678;
	SetLastError(seed);
	TEST_CHECK(!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &threadToken));
	TEST_CHECK_EQ(ERROR_NO_TOKEN, GetLastError());
	TEST_CHECK(threadToken == NULL);
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	SetLastError(seed);
	TEST_CHECK(!ImpersonateLoggedOnUser(event));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(event));
	SetLastError(seed);
	TEST_CHECK(!ImpersonateLoggedOnUser(NULL));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	const DWORD unsupported[] = {TOKEN_QUERY_SOURCE, TOKEN_IMPERSONATE,		TOKEN_ADJUST_PRIVILEGES,
								 GENERIC_READ,		 GENERIC_WRITE,			GENERIC_EXECUTE,
								 GENERIC_ALL,		 ACCESS_SYSTEM_SECURITY};
	for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
		HANDLE token = (HANDLE)(ULONG_PTR)0x12345678;
		SetLastError(seed);
		TEST_CHECK(!OpenProcessToken(GetCurrentProcess(), unsupported[i], &token));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(token == NULL);
		duplicate(base, unsupported[i], SecurityIdentification, TokenImpersonation, ERROR_NOT_SUPPORTED, FALSE);
	}
	duplicate(base, TOKEN_QUERY, SecurityIdentification, (TOKEN_TYPE)0, ERROR_INVALID_PARAMETER, FALSE);
	duplicate(base, TOKEN_QUERY, SecurityIdentification, (TOKEN_TYPE)3, ERROR_INVALID_PARAMETER, FALSE);
	SECURITY_DESCRIPTOR descriptor;
	TEST_CHECK(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
	HANDLE output = (HANDLE)(ULONG_PTR)0x12345678;
	SetLastError(seed);
	TEST_CHECK(!DuplicateTokenEx(base, TOKEN_QUERY, &attributes, SecurityIdentification, TokenImpersonation, &output));
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	TEST_CHECK(output == NULL);
	HANDLE process = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, PROCESS_VM_READ,
							   FALSE, 0));
	SetLastError(seed);
	TEST_CHECK(!OpenProcessToken(process, TOKEN_QUERY, &output));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(output == NULL);
	TEST_CHECK(CloseHandle(process));
}

int main(int argc, char **argv) {
	HANDLE base = NULL;
	SetLastError(seed);
	TEST_CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &base));
	TEST_CHECK_EQ(seed, GetLastError());
	check_queries(base, TokenPrimary, SecurityAnonymous, seed);
	const DWORD rights[] = {TOKEN_QUERY, TOKEN_DUPLICATE, TOKEN_QUERY | TOKEN_DUPLICATE, MAXIMUM_ALLOWED};
	for (unsigned i = 0; i < sizeof(rights) / sizeof(rights[0]); ++i) {
		HANDLE token = NULL;
		SetLastError(seed);
		TEST_CHECK(OpenProcessToken(GetCurrentProcess(), rights[i], &token));
		TEST_CHECK_EQ(seed, GetLastError());
		DWORD error = rights[i] == TOKEN_DUPLICATE ? ERROR_ACCESS_DENIED : seed;
		check_queries(token, TokenPrimary, SecurityAnonymous, error);
		HANDLE copy = duplicate(token, 0, SecurityIdentification, TokenImpersonation,
								rights[i] == TOKEN_QUERY ? ERROR_ACCESS_DENIED : seed, FALSE);
		if (copy) {
			check_queries(copy, TokenImpersonation, SecurityIdentification, error);
			TEST_CHECK(CloseHandle(copy));
		}
		TEST_CHECK(CloseHandle(token));
	}
	HANDLE denied = (HANDLE)(ULONG_PTR)0x12345678;
	SetLastError(seed);
	TEST_CHECK(!OpenProcessToken(GetCurrentProcess(), 0, &denied));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(denied == NULL);
	for (unsigned kind = TokenPrimary; kind <= TokenImpersonation; ++kind) {
		for (unsigned level = SecurityAnonymous; level <= SecurityDelegation; ++level) {
			HANDLE copy = duplicate(base, TOKEN_QUERY | TOKEN_DUPLICATE, (SECURITY_IMPERSONATION_LEVEL)level,
									(TOKEN_TYPE)kind, seed, FALSE);
			check_queries(copy, (TOKEN_TYPE)kind, (SECURITY_IMPERSONATION_LEVEL)level, seed);
			TEST_CHECK(CloseHandle(copy));
		}
	}
	HANDLE identity =
		duplicate(base, TOKEN_QUERY | TOKEN_DUPLICATE, SecurityIdentification, TokenImpersonation, seed, TRUE);
	HANDLE lowered = duplicate(identity, TOKEN_QUERY, SecurityAnonymous, TokenImpersonation, seed, FALSE);
	check_queries(lowered, TokenImpersonation, SecurityAnonymous, seed);
	TEST_CHECK(CloseHandle(lowered));
	duplicate(identity, TOKEN_QUERY, SecurityImpersonation, TokenImpersonation, ERROR_BAD_IMPERSONATION_LEVEL, FALSE);
	duplicate(base, TOKEN_QUERY, (SECURITY_IMPERSONATION_LEVEL)4, TokenImpersonation, ERROR_BAD_IMPERSONATION_LEVEL,
			  FALSE);
	duplicate(base, TOKEN_QUERY, (SECURITY_IMPERSONATION_LEVEL)~0u, TokenImpersonation, ERROR_BAD_IMPERSONATION_LEVEL,
			  FALSE);
	HANDLE primary = duplicate(identity, TOKEN_QUERY, SecurityImpersonation, TokenPrimary, seed, FALSE);
	check_queries(primary, TokenPrimary, SecurityAnonymous, seed);
	TEST_CHECK(CloseHandle(primary));
	TEST_CHECK(CloseHandle(identity));
	const DWORD reduced[] = {0, TOKEN_QUERY, TOKEN_DUPLICATE};
	for (unsigned i = 0; i < sizeof(reduced) / sizeof(reduced[0]); ++i) {
		HANDLE copy = NULL;
		TEST_CHECK(DuplicateHandle(GetCurrentProcess(), base, GetCurrentProcess(), &copy, reduced[i], FALSE, 0));
		check_queries(copy, TokenPrimary, SecurityAnonymous, reduced[i] & TOKEN_QUERY ? seed : ERROR_ACCESS_DENIED);
		HANDLE token = duplicate(copy, TOKEN_QUERY, SecurityIdentification, TokenImpersonation,
								 reduced[i] & TOKEN_DUPLICATE ? seed : ERROR_ACCESS_DENIED, FALSE);
		if (token) {
			check_queries(token, TokenImpersonation, SecurityIdentification, seed);
			TEST_CHECK(CloseHandle(token));
		}
		TEST_CHECK(CloseHandle(copy));
	}
	HANDLE process = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
							   PROCESS_QUERY_INFORMATION, FALSE, 0));
	HANDLE token = NULL;
	TEST_CHECK(OpenProcessToken(process, TOKEN_QUERY, &token));
	check_queries(token, TokenPrimary, SecurityAnonymous, seed);
	TEST_CHECK(CloseHandle(token));
	TEST_CHECK(CloseHandle(process));
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	check_queries(event, TokenPrimary, SecurityAnonymous, ERROR_INVALID_HANDLE);
	duplicate(event, TOKEN_QUERY, SecurityIdentification, TokenImpersonation, ERROR_INVALID_HANDLE, FALSE);
	TEST_CHECK(CloseHandle(event));
	check_queries(NULL, TokenPrimary, SecurityAnonymous, ERROR_INVALID_HANDLE);
	duplicate(NULL, TOKEN_QUERY, SecurityIdentification, TokenImpersonation, ERROR_INVALID_HANDLE, FALSE);
	if (argc > 1 && strcmp(argv[1], "unsupported") == 0)
		check_gap_cases(base);
	TEST_CHECK(CloseHandle(base));
	check_queries(base, TokenPrimary, SecurityAnonymous, ERROR_INVALID_HANDLE);
	duplicate(base, TOKEN_QUERY, SecurityIdentification, TokenImpersonation, ERROR_INVALID_HANDLE, FALSE);
	return 0;
}
