#include "common.h"
#include "setup.h"

#include <cstdio>
#include <ctime>
#include <pthread.h>

thread_local TEB *currentThreadTeb = nullptr;

namespace {
uintptr_t slot() {
	uintptr_t result;
	__asm__ volatile("movq %%gs:0x60, %0" : "=r"(result));
	return result;
}

uint64_t errorSlot() {
	uint64_t result;
	__asm__ volatile("movq %%gs:0x68, %0" : "=r"(result));
	return result;
}

void writeErrorSlot(uint64_t value) { __asm__ volatile("movq %0, %%gs:0x68" : : "r"(value) : "memory"); }

void *check(void *) {
	time_t now = 1700000000;
	tm *cached = localtime(&now);
	if (!cached)
		return reinterpret_cast<void *>(1);
	const tm expected = *cached;
	const auto original = slot();
	const auto originalError = errorSlot();
	PEB peb{};
	TEB teb{};
	teb.Peb = toGuestPtr(&peb);
	if (!tebThreadSetup(&teb))
		return reinterpret_cast<void *>(2);
	currentThreadTeb = &teb;
	for (unsigned i = 0; i < 100; ++i) {
		const uint64_t hostMarker = 0x1234567800000000ULL + i;
		teb.LastErrorValue = i + 1;
		writeErrorSlot(hostMarker);
		enterGuestContext(&teb);
		enterGuestContext(&teb);
		const bool guest = slot() == toGuestPtr(&peb) && errorSlot() == i + 1;
		writeErrorSlot(i + 2);
		const bool host = enterHostContext() == &teb && slot() == original && errorSlot() == hostMarker &&
						  teb.LastErrorValue == i + 2;
		teb.LastErrorValue = i + 3;
		const bool repeated = enterHostContext() == &teb && slot() == original && errorSlot() == hostMarker &&
							  teb.LastErrorValue == i + 3;
		enterGuestContext(&teb);
		const bool updated = errorSlot() == i + 3;
		teb.LastErrorValue = i + 4;
		const bool direct = enterHostContext() == &teb && errorSlot() == hostMarker && teb.LastErrorValue == i + 4;
		writeErrorSlot(originalError);
		tm *after = localtime(&now);
		if (!guest || !host || !repeated || !updated || !direct || after != cached ||
			after->tm_year != expected.tm_year || after->tm_yday != expected.tm_yday) {
			tebThreadTeardown(&teb);
			currentThreadTeb = nullptr;
			return reinterpret_cast<void *>(3);
		}
	}
	enterGuestContext(&teb);
	writeErrorSlot(123);
	tebThreadEmergencyTeardown();
	const bool restored =
		slot() == original && errorSlot() == originalError && teb.LastErrorValue == 123 && !teb.GuestContextActive;
	currentThreadTeb = nullptr;
	return restored ? nullptr : reinterpret_cast<void *>(4);
}
} // namespace

int main() {
	if (check(nullptr))
		return 1;
	pthread_t threads[8];
	for (auto &thread : threads)
		if (pthread_create(&thread, nullptr, check, nullptr))
			return 2;
	for (auto thread : threads) {
		void *result;
		if (pthread_join(thread, &result) || result) {
			std::fputs("Thread storage restoration failed\n", stderr);
			return 3;
		}
	}
	return 0;
}
