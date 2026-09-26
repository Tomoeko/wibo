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

void *check(void *) {
	time_t now = 1700000000;
	tm *cached = localtime(&now);
	if (!cached)
		return reinterpret_cast<void *>(1);
	const tm expected = *cached;
	const auto original = slot();
	PEB peb{};
	TEB teb{};
	teb.Peb = toGuestPtr(&peb);
	if (!tebThreadSetup(&teb))
		return reinterpret_cast<void *>(2);
	currentThreadTeb = &teb;
	for (unsigned i = 0; i < 100; ++i) {
		enterGuestContext(&teb);
		enterGuestContext(&teb);
		const bool guest = slot() == toGuestPtr(&peb);
		const bool host = enterHostContext() == &teb && slot() == original;
		const bool repeated = enterHostContext() == &teb && slot() == original;
		tm *after = localtime(&now);
		if (!guest || !host || !repeated || after != cached || after->tm_year != expected.tm_year ||
			after->tm_yday != expected.tm_yday) {
			tebThreadTeardown(&teb);
			currentThreadTeb = nullptr;
			return reinterpret_cast<void *>(3);
		}
	}
	enterGuestContext(&teb);
	tebThreadEmergencyTeardown();
	const bool restored = slot() == original && !teb.GuestContextActive;
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
