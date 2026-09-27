#include "bcrypt.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"
#include "strutil.h"

#include "advapi32/md5.h"
#include "advapi32/sha1.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <sys/random.h>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace {

constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xc0000017);
constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xc0000023);
constexpr ULONG kKnownOpenFlags = 0x1 | 0x8 | 0x20; // Dispatch, HMAC, and reusable hashing.
constexpr size_t kMaxIdentifierNameUnits = 256;

struct HashImplementation {
	size_t contextSize;
	ULONG digestSize;
	void (*initialize)(void *);
	void (*update)(void *, const void *, size_t);
	void (*finish)(void *, unsigned char *);
};

void initializeMd5(void *context) { MD5_Init(static_cast<MD5_CTX *>(context)); }
void updateMd5(void *context, const void *data, size_t length) {
	MD5_Update(static_cast<MD5_CTX *>(context), data, length);
}
void finishMd5(void *context, unsigned char *digest) { MD5_Final(digest, static_cast<MD5_CTX *>(context)); }
void initializeSha1(void *context) { sha1_init(static_cast<sha1_context *>(context)); }
void updateSha1(void *context, const void *data, size_t length) {
	sha1_update(static_cast<sha1_context *>(context), data, length);
}
void finishSha1(void *context, unsigned char *digest) { sha1_finalize(static_cast<sha1_context *>(context), digest); }

constexpr HashImplementation kMd5Implementation{sizeof(MD5_CTX), 16, initializeMd5, updateMd5, finishMd5};
constexpr HashImplementation kSha1Implementation{sizeof(sha1_context), SHA1_SIZE, initializeSha1, updateSha1,
												 finishSha1};

#ifdef __APPLE__
void initializeSha256(void *context) { CC_SHA256_Init(static_cast<CC_SHA256_CTX *>(context)); }
void updateSha256(void *context, const void *data, size_t length) {
	const auto *bytes = static_cast<const unsigned char *>(data);
	while (length) {
		const size_t chunk = std::min(length, static_cast<size_t>(std::numeric_limits<CC_LONG>::max()));
		CC_SHA256_Update(static_cast<CC_SHA256_CTX *>(context), bytes, static_cast<CC_LONG>(chunk));
		bytes += chunk;
		length -= chunk;
	}
}
void finishSha256(void *context, unsigned char *digest) {
	CC_SHA256_Final(digest, static_cast<CC_SHA256_CTX *>(context));
}
constexpr HashImplementation kSha256Implementation{sizeof(CC_SHA256_CTX), CC_SHA256_DIGEST_LENGTH, initializeSha256,
												   updateSha256, finishSha256};
#endif

struct AlgorithmProvider {
	const HashImplementation *hash;
};

std::mutex g_algorithmMutex;
std::unordered_map<BCRYPT_ALG_HANDLE, std::shared_ptr<const AlgorithmProvider>> g_algorithms;
// Tokens are opaque and never reused, including after an algorithm is closed.
BCRYPT_ALG_HANDLE g_nextAlgorithmHandle = 0x10000;

bool readIdentifierName(LPCWSTR name, std::u16string &value) {
	const size_t units = wstrnlen(name, kMaxIdentifierNameUnits);
	value.assign(name, name + units);
	return units != kMaxIdentifierNameUnits;
}

std::string displayIdentifierName(const std::u16string &value) {
	std::string display;
	return utf16ToUtf8(value, display) ? display : "<invalid UTF-16>";
}

constexpr ULONG BCRYPT_RNG_USE_ENTROPY_IN_BUFFER = 0x00000001;
constexpr ULONG BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x00000002;

bool fillWithSystemRandom(PUCHAR buffer, size_t length) {
#ifdef __APPLE__
	arc4random_buf(buffer, length);
#else
	while (length > 0) {
		ssize_t written = getrandom(buffer, length, 0);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (written == 0)
			continue;
		buffer += written;
		length -= static_cast<size_t>(written);
	}
#endif
	return true;
}

} // namespace

namespace bcrypt {

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation,
											ULONG dwFlags) {
	HOST_CONTEXT_GUARD();
	if (!phAlgorithm || !pszAlgId) {
		DEBUG_LOG("BCryptOpenAlgorithmProvider(%p, %p, %p, 0x%x) -> invalid parameter\n", phAlgorithm, pszAlgId,
				  pszImplementation, dwFlags);
		return STATUS_INVALID_PARAMETER;
	}
	std::u16string identifier, provider;
	const bool hasIdentifier = readIdentifierName(pszAlgId, identifier);
	const bool hasProvider = !pszImplementation || readIdentifierName(pszImplementation, provider);
	DEBUG_LOG("BCryptOpenAlgorithmProvider(%p, '%s', '%s', 0x%x)\n", phAlgorithm,
			  displayIdentifierName(identifier).c_str(),
			  pszImplementation ? displayIdentifierName(provider).c_str() : "<default>", dwFlags);
	if (dwFlags & ~kKnownOpenFlags)
		return STATUS_NOT_IMPLEMENTED;
	if (dwFlags)
		return STATUS_NOT_SUPPORTED;
	if (!hasIdentifier || !hasProvider || (pszImplementation && provider != u"Microsoft Primitive Provider"))
		return STATUS_NOT_IMPLEMENTED;
	const HashImplementation *implementation = nullptr;
	if (identifier == u"MD5")
		implementation = &kMd5Implementation;
	else if (identifier == u"SHA1")
		implementation = &kSha1Implementation;
	else if (identifier == u"SHA256") {
#ifdef __APPLE__
		implementation = &kSha256Implementation;
#else
		return STATUS_NOT_SUPPORTED;
#endif
	} else
		return STATUS_NOT_IMPLEMENTED;
	// This uses local primitives, without host CNG registration or configuration.
	auto providerObject = std::make_shared<const AlgorithmProvider>(AlgorithmProvider{implementation});
	std::lock_guard lock(g_algorithmMutex);
	if (g_nextAlgorithmHandle == std::numeric_limits<BCRYPT_ALG_HANDLE>::max())
		return kStatusNoMemory;
	const BCRYPT_ALG_HANDLE handle = g_nextAlgorithmHandle++;
	g_algorithms.emplace(handle, std::move(providerObject));
	std::memcpy(phAlgorithm, &handle, sizeof(handle));
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("BCryptCloseAlgorithmProvider(0x%llx, 0x%x)\n", static_cast<unsigned long long>(hAlgorithm), dwFlags);
	if (dwFlags)
		return STATUS_NOT_SUPPORTED;
	std::lock_guard lock(g_algorithmMutex);
	return g_algorithms.erase(hAlgorithm) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS WINAPI BCryptGetProperty(BCRYPT_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput, ULONG cbOutput,
								  ULONG *pcbResult, ULONG dwFlags) {
	HOST_CONTEXT_GUARD();
	std::shared_ptr<const AlgorithmProvider> provider;
	{
		std::lock_guard lock(g_algorithmMutex);
		const auto found = g_algorithms.find(hObject);
		if (found == g_algorithms.end())
			return STATUS_INVALID_HANDLE;
		provider = found->second;
	}
	if (!pszProperty || !pcbResult) {
		DEBUG_LOG("BCryptGetProperty(0x%llx, %p, %p, %u, %p, 0x%x) -> invalid parameter\n",
				  static_cast<unsigned long long>(hObject), pszProperty, pbOutput, cbOutput, pcbResult, dwFlags);
		return STATUS_INVALID_PARAMETER;
	}
	std::u16string property;
	const bool hasProperty = readIdentifierName(pszProperty, property);
	DEBUG_LOG("BCryptGetProperty(0x%llx, '%s', %p, %u, %p, 0x%x)\n", static_cast<unsigned long long>(hObject),
			  displayIdentifierName(property).c_str(), pbOutput, cbOutput, pcbResult, dwFlags);
	if (dwFlags || !hasProperty || property != u"HashDigestLength")
		return STATUS_NOT_SUPPORTED;
	static_assert(sizeof(ULONG) == 4);
	constexpr ULONG required = sizeof(ULONG);
	std::memcpy(pcbResult, &required, sizeof(required));
	if (cbOutput < required)
		return kStatusBufferTooSmall;
	if (pbOutput)
		std::memcpy(pbOutput, &provider->hash->digestSize, sizeof(provider->hash->digestSize));
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("BCryptGenRandom(0x%llx, %p, %u, %u)\n", static_cast<unsigned long long>(hAlgorithm), pbBuffer, cbBuffer,
			  dwFlags);
	if (pbBuffer == nullptr && cbBuffer != 0)
		return STATUS_INVALID_HANDLE;

	if (hAlgorithm != GUEST_NULL)
		return STATUS_NOT_IMPLEMENTED;

	if ((dwFlags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
		return STATUS_INVALID_HANDLE;

	ULONG allowedFlags = BCRYPT_RNG_USE_ENTROPY_IN_BUFFER | BCRYPT_USE_SYSTEM_PREFERRED_RNG;
	if ((dwFlags & ~allowedFlags) != 0)
		return STATUS_INVALID_PARAMETER;

	if (cbBuffer == 0)
		return STATUS_SUCCESS;

	std::vector<unsigned char> entropy;
	if ((dwFlags & BCRYPT_RNG_USE_ENTROPY_IN_BUFFER) && pbBuffer != nullptr)
		entropy.assign(pbBuffer, pbBuffer + cbBuffer);

	if (!fillWithSystemRandom(pbBuffer, cbBuffer))
		return STATUS_UNEXPECTED_IO_ERROR;

	if (!entropy.empty()) {
		for (size_t i = 0; i < entropy.size(); ++i)
			pbBuffer[i] ^= entropy[i];
	}
	return STATUS_SUCCESS;
}

BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ProcessPrng(%p, %lu)\n", pbData, cbData);
	if (pbData == nullptr && cbData != 0)
		return FALSE;
	return fillWithSystemRandom(pbData, cbData);
}

} // namespace bcrypt

#include "bcrypt_trampolines.h"

extern const wibo::ModuleStub lib_bcrypt = {
	(const char *[]){
		"bcrypt",
		"bcryptprimitives",
		nullptr,
	},
	bcryptThunkByName,
	nullptr,
};
