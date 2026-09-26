#include "common.h"
#include "setup.h"

#include "types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#ifdef WIBO_GUEST_64
#include <pthread.h>
#endif
#include <mutex>

#ifndef WIBO_GUEST_64
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#endif

// https://github.com/apple/darwin-libpthread/blob/03c4628c8940cca6fd6a82957f683af804f62e7f/private/tsd_private.h#L92-L97
#define _PTHREAD_TSD_SLOT_RESERVED_WIN64 6

#ifdef WIBO_GUEST_64
// MSVC emits direct GS:[0x58] accesses for static TLS. Wine's established
// macOS x86_64 model mirrors this pointer into the corresponding native TSD
// slot while retaining macOS's GS base. Slot 6 is reserved by macOS for the
// Win64 TEB self pointer at GS:[0x30].
constexpr uint32_t kWin64StaticTlsTsdSlot = 0x58 / sizeof(void *);
// GS:[0x60] overlaps Darwin's libc localtime cache rather than a reserved slot.
// Publish the guest PEB only while executing guest code and restore the native
// cache before any host API, callback return, or pthread teardown.
constexpr uint32_t kWin64PebTsdSlot = 0x60 / sizeof(void *);
// Inline last-error access uses GS:[0x68]. Its native TSD slot must also be
// saved across guest execution rather than replaced for the thread lifetime.
constexpr uint32_t kWin64LastErrorTsdSlot = 0x68 / sizeof(void *);

// Darwin publishes this stable layout SPI for tools that need to translate a
// pthread_t into its direct TSD base. Using it avoids Mach thread_info and
// mach_port_deallocate traps in translated worker startup.
struct PthreadLayoutOffsets {
	uint16_t version;
	uint16_t pthreadTsdBaseOffset;
	uint16_t pthreadTsdBaseAddressOffset;
	uint16_t pthreadTsdEntrySize;
};
extern "C" const PthreadLayoutOffsets pthread_layout_offsets;
#endif

#ifndef WIBO_GUEST_64
// Implemented in setup.S
extern "C" int installSelectors(TEB *teb);
#endif

namespace {

inline void writeTsdSlot(uint32_t slot, uint64_t val) {
	// mov qword ptr gs:[slot*8], val
	*(volatile uint64_t __seg_gs *)(slot * sizeof(void *)) = val;
}

#ifdef WIBO_GUEST_64
void *currentTsdBase() {
	if (pthread_layout_offsets.pthreadTsdEntrySize != sizeof(void *)) {
		return nullptr;
	}
	auto *self = reinterpret_cast<uint8_t *>(pthread_self());
	if (pthread_layout_offsets.pthreadTsdBaseOffset != 0) {
		return self + pthread_layout_offsets.pthreadTsdBaseOffset;
	}
	if (pthread_layout_offsets.pthreadTsdBaseAddressOffset != 0) {
		return *reinterpret_cast<void **>(self + pthread_layout_offsets.pthreadTsdBaseAddressOffset);
	}
	return nullptr;
}

void writeTsdSlotAtBase(GUEST_PTR tsdBase, uint32_t slot, uint64_t value) {
	if (!tsdBase) {
		return;
	}
	auto *slots = reinterpret_cast<volatile uint64_t *>(static_cast<uintptr_t>(tsdBase));
	slots[slot] = value;
}
#endif

#ifndef WIBO_GUEST_64
std::mutex g_tebSetupMutex;
uint16_t g_codeSelector = 0;
uint16_t g_dataSelector = 0;
constexpr int kMaxLdtEntries = 8192;
constexpr int kBitsPerWord = 32;
std::array<uint32_t, kMaxLdtEntries / kBitsPerWord> g_ldtBitmap{};
bool g_ldtBitmapInitialized = false;
int g_ldtHint = 1;

inline ldt_entry newLdtEntry() {
	ldt_entry entry; // NOLINT(cppcoreguidelines-pro-type-member-init)
	// Must memset to zero to avoid uninitialized padding bytes
	std::memset(&entry, 0, sizeof(ldt_entry));
	return entry;
}

inline ldt_entry createLdtEntry(uint32_t base, uint32_t size, bool code) {
	uint32_t limit;
	uint8_t granular;
	if (size > 0xFFFFF) {
		limit = (size - 1) >> 12;
		granular = DESC_GRAN_PAGE;
	} else {
		limit = size - 1;
		granular = DESC_GRAN_BYTE;
	}
	ldt_entry entry = newLdtEntry();
	entry.code.limit00 = static_cast<uint16_t>(limit);
	entry.code.base00 = static_cast<uint16_t>(base);
	entry.code.base16 = static_cast<uint8_t>(base >> 16);
	entry.code.type = code ? DESC_CODE_READ : DESC_DATA_WRITE;
	entry.code.dpl = USER_PRIVILEGE;
	entry.code.present = 1;
	entry.code.limit16 = static_cast<uint8_t>(limit >> 16);
	entry.code.opsz = DESC_CODE_32B;
	entry.code.granular = granular;
	entry.code.base24 = static_cast<uint8_t>(base >> 24);
	return entry;
}

constexpr int createSelector(int entryNumber) { return (entryNumber << 3) | 0x4 /* TI=1 */ | USER_PRIVILEGE; }

inline bool isLdtEntryValid(int entry) { return entry >= 0 && entry < kMaxLdtEntries; }

inline void markLdtEntryUsed(int entry) {
	if (!isLdtEntryValid(entry)) {
		return;
	}
	g_ldtBitmap[entry / kBitsPerWord] |= (1u << (entry % kBitsPerWord));
}

inline void markLdtEntryFree(int entry) {
	if (!isLdtEntryValid(entry)) {
		return;
	}
	g_ldtBitmap[entry / kBitsPerWord] &= ~(1u << (entry % kBitsPerWord));
}

inline bool isLdtEntryUsed(int entry) {
	if (!isLdtEntryValid(entry)) {
		return true;
	}
	return (g_ldtBitmap[entry / kBitsPerWord] & (1u << (entry % kBitsPerWord))) != 0;
}

bool initializeLdtBitmapLocked() {
	if (g_ldtBitmapInitialized) {
		return true;
	}
	ldt_entry unused{};
	int count = i386_get_ldt(0, &unused, 1);
	if (count < 0) {
		DEBUG_LOG("setup_darwin: i386_get_ldt failed during bitmap init (%d), assuming empty table\n", count);
		return false;
	}
	if (count > kMaxLdtEntries) {
		DEBUG_LOG("setup_darwin: i386_get_ldt returned too many entries (%d), truncating to %d\n", count,
				  kMaxLdtEntries);
		count = kMaxLdtEntries;
	}
	for (int i = 0; i < count; ++i) {
		markLdtEntryUsed(i);
	}
	g_ldtBitmapInitialized = true;
	return true;
}

int allocateLdtEntryLocked() {
	if (!initializeLdtBitmapLocked()) {
		errno = ENOSPC;
		return -1;
	}
	auto tryAllocate = [&](int start) -> int {
		for (int entry = start; entry < kMaxLdtEntries; ++entry) {
			if (!isLdtEntryUsed(entry)) {
				markLdtEntryUsed(entry);
				g_ldtHint = entry + 1;
				if (g_ldtHint >= kMaxLdtEntries) {
					g_ldtHint = 1;
				}
				DEBUG_LOG("setup_darwin: Allocating LDT entry %d\n", entry);
				return entry;
			}
		}
		return -1;
	};
	int entry = tryAllocate(std::max(g_ldtHint, 1));
	if (entry >= 0) {
		return entry;
	}
	entry = tryAllocate(1);
	if (entry >= 0) {
		return entry;
	}
	errno = ENOSPC;
	return -1;
}

void freeLdtEntryLocked(int entryNumber) {
	if (!g_ldtBitmapInitialized || !isLdtEntryValid(entryNumber)) {
		return;
	}
	markLdtEntryFree(entryNumber);
	if (entryNumber < g_ldtHint) {
		g_ldtHint = std::max(entryNumber, 1);
	}
}

bool segmentSetupLocked(TEB *teb) {
	// Create code LDT entry
	if (g_codeSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		ldt_entry codeLdt = createLdtEntry(0, 0xFFFFFFFF, true);
		int ret = i386_set_ldt(entryNumber, &codeLdt, 1);
		if (ret < 0) {
			freeLdtEntryLocked(entryNumber);
			return false;
		} else if (ret != entryNumber) {
			freeLdtEntryLocked(entryNumber);
			errno = EALREADY;
			return false;
		}
		g_codeSelector = createSelector(ret);
		DEBUG_LOG("setup_darwin: Code LDT selector %x\n", g_codeSelector);
	}
	// Create data LDT entry
	if (g_dataSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		ldt_entry dataLdt = createLdtEntry(0, 0xFFFFFFFF, false);
		int ret = i386_set_ldt(entryNumber, &dataLdt, 1);
		if (ret < 0) {
			freeLdtEntryLocked(entryNumber);
			return false;
		} else if (ret != entryNumber) {
			freeLdtEntryLocked(entryNumber);
			errno = EALREADY;
			return false;
		}
		g_dataSelector = createSelector(ret);
		DEBUG_LOG("setup_darwin: Data LDT selector %x\n", g_dataSelector);
	}
	teb->CodeSelector = g_codeSelector;
	teb->DataSelector = g_dataSelector;
	return true;
}
#endif

} // namespace

#ifdef WIBO_GUEST_64
TEB *enterHostContext() {
	auto *teb = reinterpret_cast<TEB *>(*(volatile uint64_t __seg_gs *)0x30);
	if (teb && teb->GuestContextActive) {
		const auto error = *(volatile uint32_t __seg_gs *)0x68;
		// Preserve a write through the TEB self pointer when the mirror was unchanged.
		if (error != teb->MirroredLastErrorValue)
			teb->LastErrorValue = error;
		writeTsdSlot(kWin64LastErrorTsdSlot, teb->HostLastErrorTsd);
		writeTsdSlot(kWin64PebTsdSlot, teb->HostLocalTimeTsd);
		teb->GuestContextActive = false;
	}
	return teb;
}

void enterGuestContext(TEB *teb) {
	if (teb && !teb->GuestContextActive) {
		teb->HostLocalTimeTsd = *(volatile uint64_t __seg_gs *)0x60;
		teb->HostLastErrorTsd = *(volatile uint64_t __seg_gs *)0x68;
		teb->MirroredLastErrorValue = teb->LastErrorValue;
		writeTsdSlot(kWin64LastErrorTsdSlot, teb->LastErrorValue);
		writeTsdSlot(kWin64PebTsdSlot, teb->Peb);
		teb->GuestContextActive = true;
	}
}

bool tebThreadSetup(TEB *teb) {
	if (!teb) {
		return false;
	}
	if (!teb->HostTsdBase) {
		teb->HostTsdBase = toGuestPtr(currentTsdBase());
		if (!teb->HostTsdBase) {
			return false;
		}
	}
	writeTsdSlot(_PTHREAD_TSD_SLOT_RESERVED_WIN64, reinterpret_cast<uintptr_t>(teb));
	writeTsdSlot(kWin64StaticTlsTsdSlot, static_cast<uint64_t>(teb->ThreadLocalStoragePointer));
	return true;
}

bool tebThreadTeardown(TEB *teb) {
	enterHostContext();
	if (teb) {
		writeTsdSlot(kWin64StaticTlsTsdSlot, 0);
		writeTsdSlot(_PTHREAD_TSD_SLOT_RESERVED_WIN64, 0);
		// The native pthread storage ceases to be a valid cross-thread target
		// once this thread exits. TLS cleanup may occur later with handle cleanup.
		teb->HostTsdBase = GUEST_NULL;
	}
	return true;
}

void tebThreadEmergencyTeardown() {
	enterHostContext();
	// GS always remains the native pthread base, including in signal handlers.
	writeTsdSlot(kWin64StaticTlsTsdSlot, 0);
	writeTsdSlot(_PTHREAD_TSD_SLOT_RESERVED_WIN64, 0);
}

void tebThreadTlsPointerChanged(TEB *teb) {
	if (!teb) {
		return;
	}
	writeTsdSlotAtBase(teb->HostTsdBase, kWin64StaticTlsTsdSlot,
					   static_cast<uint64_t>(teb->ThreadLocalStoragePointer));
}

TEB *currentTebForGuestTransition() { return currentThreadTeb; }
#else
bool tebThreadSetup(TEB *teb) {
	if (!teb) {
		return false;
	}
	std::lock_guard lk(g_tebSetupMutex);
	// Perform global segment setup if not already done
	if (!segmentSetupLocked(teb)) {
		return false;
	}
	int entryNumber = allocateLdtEntryLocked();
	if (entryNumber < 0) {
		return false;
	}
	uintptr_t tebBase = reinterpret_cast<uintptr_t>(teb);
	if (tebBase > 0xFFFFFFFF) {
		fprintf(stderr, "setup_darwin: TEB base address exceeds 32-bit limit\n");
		freeLdtEntryLocked(entryNumber);
		errno = EINVAL;
		return false;
	}
	// Store the TEB base address in the reserved slot for Windows 64-bit (gs:[0x30])
	writeTsdSlot(_PTHREAD_TSD_SLOT_RESERVED_WIN64, static_cast<uint32_t>(tebBase));
	// Rosetta 2 requires size 0x1000 (limit 0xFFF) specifically
	ldt_entry fsLdt = createLdtEntry(static_cast<uint32_t>(tebBase), 0x1000, false);
	int ret = i386_set_ldt(entryNumber, &fsLdt, 1);
	if (ret < 0) {
		freeLdtEntryLocked(entryNumber);
		return false;
	} else if (ret != entryNumber) {
		freeLdtEntryLocked(entryNumber);
		errno = EALREADY;
		return false;
	}
	teb->CurrentFsSelector = createSelector(entryNumber);
	DEBUG_LOG("setup_darwin: Installing cs %d, ds %d, fs %d\n", teb->CodeSelector, teb->DataSelector,
			  teb->CurrentFsSelector);
	installSelectors(teb);
	return true;
}

bool tebThreadTeardown(TEB *teb) {
	if (!teb) {
		return true;
	}
	std::lock_guard lk(g_tebSetupMutex);
	writeTsdSlot(_PTHREAD_TSD_SLOT_RESERVED_WIN64, 0);
	uint16_t selector = teb->CurrentFsSelector;
	if (selector == 0) {
		return true;
	}
	int entryNumber = selector >> 3;
	ldt_entry entry = newLdtEntry();
	int ret = i386_set_ldt(entryNumber, &entry, 1);
	if (ret < 0) {
		return false;
	}
	freeLdtEntryLocked(entryNumber);
	teb->CurrentFsSelector = 0;
	return true;
}
#endif
