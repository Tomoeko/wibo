#define _WIN32_WINNT 0x0600
#include "test_assert.h"
#include <windows.h>

typedef void(WINAPI *FlushWriteBuffersFn)(void);

enum {
	visibility_iterations = 512,
	concurrent_iterations = 64,
	concurrent_workers = 2,
	spawn_iterations = 4,
	wait_ms = 5000,
	spawn_phase_ms = 5000,
	spawn_cleanup_ms = 5000,
	spawn_join_ms = wait_ms + spawn_phase_ms + spawn_cleanup_ms + 1000
};

_Static_assert(__atomic_always_lock_free(sizeof(LONG), 0), "The observations need lock-free word atomics");

struct VisibilityState {
	LONG epoch;
	LONG completed;
	LONG stop;
	LONG peer_observation;
};

// Control handshakes stay outside the two store/load observations.
static struct VisibilityState visibility;
static LONG first_word __attribute__((aligned(64)));
static LONG second_word __attribute__((aligned(64)));
static FlushWriteBuffersFn flush_write_buffers;

static BOOL await_value(LONG *word, LONG value) {
	const DWORD start = GetTickCount();
	unsigned spins = 0;
	while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != value) {
		if (__atomic_load_n(&visibility.stop, __ATOMIC_RELAXED))
			return FALSE;
		if (++spins % 1024 == 0) {
			if (GetTickCount() - start >= wait_ms)
				return FALSE;
			Sleep(0);
		}
	}
	return TRUE;
}

static DWORD WINAPI visibility_worker(PVOID unused) {
	(void)unused;
	for (LONG iteration = 1; iteration <= visibility_iterations; ++iteration) {
		if (!await_value(&visibility.epoch, iteration))
			return WAIT_TIMEOUT;
		__atomic_store_n(&first_word, 1, __ATOMIC_RELAXED);
		__asm__ volatile("" ::: "memory");
		const LONG observed = __atomic_load_n(&second_word, __ATOMIC_RELAXED);
		visibility.peer_observation = observed;
		__atomic_store_n(&visibility.completed, iteration, __ATOMIC_RELEASE);
	}
	return 0;
}

struct ConcurrentState {
	HANDLE start;
	DWORD last_error;
	unsigned completed;
	BOOL preserved_error;
};

static DWORD WINAPI concurrent_worker(PVOID argument) {
	struct ConcurrentState *state = argument;
	if (WaitForSingleObject(state->start, wait_ms) != WAIT_OBJECT_0)
		return WAIT_TIMEOUT;
	state->preserved_error = TRUE;
	for (unsigned iteration = 0; iteration < concurrent_iterations; ++iteration) {
		SetLastError(state->last_error);
		flush_write_buffers();
		if (GetLastError() != state->last_error)
			state->preserved_error = FALSE;
		++state->completed;
	}
	return 0;
}

static DWORD join_worker(HANDLE thread, DWORD timeout) {
	const DWORD wait = WaitForSingleObject(thread, timeout);
	if (wait != WAIT_OBJECT_0)
		return wait;
	DWORD result = ERROR_GEN_FAILURE;
	if (!GetExitCodeThread(thread, &result))
		return ERROR_GEN_FAILURE;
	return result;
}

struct SpawnState {
	HANDLE start;
	const char *path;
	unsigned completed;
};

static DWORD WINAPI spawn_worker(PVOID argument) {
	struct SpawnState *state = argument;
	if (WaitForSingleObject(state->start, wait_ms) != WAIT_OBJECT_0)
		return WAIT_TIMEOUT;
	const DWORD phase_start = GetTickCount();
	for (unsigned iteration = 0; iteration < spawn_iterations; ++iteration) {
		if (GetTickCount() - phase_start >= spawn_phase_ms)
			return WAIT_TIMEOUT;
		char command[2048];
		const int length = snprintf(command, sizeof(command), "\"%s\" --child", state->path);
		if (length < 0 || (size_t)length >= sizeof(command))
			return ERROR_INSUFFICIENT_BUFFER;
		STARTUPINFOA startup = {0};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION child = {0};
		if (!CreateProcessA(state->path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child)) {
			fprintf(stderr, "Child creation failed: %lu\n", (unsigned long)GetLastError());
			return ERROR_GEN_FAILURE;
		}
		DWORD result = 0;
		const DWORD elapsed = GetTickCount() - phase_start;
		const DWORD remaining = elapsed < spawn_phase_ms ? spawn_phase_ms - elapsed : 0;
		const DWORD child_wait = WaitForSingleObject(child.hProcess, remaining);
		if (child_wait != WAIT_OBJECT_0) {
			result = WAIT_TIMEOUT;
			const DWORD wait_error = child_wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
			const BOOL terminated = TerminateProcess(child.hProcess, 1);
			const DWORD termination_error = terminated ? ERROR_SUCCESS : GetLastError();
			const DWORD reaped = WaitForSingleObject(child.hProcess, spawn_cleanup_ms);
			const DWORD reap_error = reaped == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
			fprintf(stderr, "Child %lu wait=%lu error=%lu terminate=%u error=%lu reap=%lu error=%lu\n",
					(unsigned long)child.dwProcessId, (unsigned long)child_wait, (unsigned long)wait_error,
					(unsigned)terminated, (unsigned long)termination_error, (unsigned long)reaped,
					(unsigned long)reap_error);
			if (!terminated || reaped != WAIT_OBJECT_0)
				result = ERROR_GEN_FAILURE;
		} else {
			DWORD code = 1;
			if (!GetExitCodeProcess(child.hProcess, &code) || code)
				result = ERROR_GEN_FAILURE;
		}
		const BOOL closed_thread = CloseHandle(child.hThread);
		const BOOL closed_process = CloseHandle(child.hProcess);
		if (!closed_thread || !closed_process)
			result = ERROR_GEN_FAILURE;
		if (result)
			return result;
		++state->completed;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "--child") == 0)
		return 0;
	FARPROC entry = GetProcAddress(GetModuleHandleA("kernel32.dll"), "FlushProcessWriteBuffers");
	_Static_assert(sizeof(entry) == sizeof(flush_write_buffers), "Resolved entries have the function-pointer width");
	memcpy(&flush_write_buffers, &entry, sizeof(flush_write_buffers));
	TEST_CHECK(flush_write_buffers != NULL);

	HANDLE peer = CreateThread(NULL, 0, visibility_worker, NULL, 0, NULL);
	TEST_CHECK(peer != NULL);
	BOOL progressed = TRUE;
	BOOL preserved_error = TRUE;
	unsigned forbidden = 0;
	unsigned completed = 0;
	for (LONG iteration = 1; iteration <= visibility_iterations; ++iteration) {
		__atomic_store_n(&first_word, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&second_word, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&visibility.epoch, iteration, __ATOMIC_RELEASE);
		const DWORD marker = 0x52340000u + (DWORD)iteration;
		SetLastError(marker);
		__atomic_store_n(&second_word, 1, __ATOMIC_RELAXED);
		flush_write_buffers();
		const LONG observed = __atomic_load_n(&first_word, __ATOMIC_RELAXED);
		if (GetLastError() != marker)
			preserved_error = FALSE;
		if (!await_value(&visibility.completed, iteration)) {
			progressed = FALSE;
			break;
		}
		if (!observed && !visibility.peer_observation)
			++forbidden;
		++completed;
	}
	__atomic_store_n(&visibility.stop, 1, __ATOMIC_RELAXED);
	const DWORD peer_result = join_worker(peer, wait_ms);
	TEST_CHECK_EQ(0, peer_result);
	TEST_CHECK(progressed);
	TEST_CHECK(preserved_error);
	TEST_CHECK_EQ(visibility_iterations, completed);
	TEST_CHECK_EQ(0, forbidden);
	TEST_CHECK(CloseHandle(peer));

	HANDLE start = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start != NULL);
	struct ConcurrentState states[concurrent_workers];
	HANDLE threads[concurrent_workers] = {NULL, NULL};
	BOOL created = TRUE;
	for (unsigned index = 0; index < concurrent_workers; ++index) {
		states[index] = (struct ConcurrentState){start, 0x52350001u + index, 0, FALSE};
		threads[index] = CreateThread(NULL, 0, concurrent_worker, &states[index], 0, NULL);
		if (!threads[index]) {
			created = FALSE;
			break;
		}
	}
	const BOOL started = SetEvent(start);
	DWORD results[concurrent_workers] = {ERROR_GEN_FAILURE, ERROR_GEN_FAILURE};
	for (unsigned index = 0; index < concurrent_workers; ++index) {
		if (threads[index])
			results[index] = join_worker(threads[index], wait_ms);
	}
	TEST_CHECK(created);
	TEST_CHECK(started);
	for (unsigned index = 0; index < concurrent_workers; ++index) {
		TEST_CHECK_EQ(0, results[index]);
		TEST_CHECK_EQ(concurrent_iterations, states[index].completed);
		TEST_CHECK(states[index].preserved_error);
		TEST_CHECK(CloseHandle(threads[index]));
	}
	TEST_CHECK(CloseHandle(start));

	char path[1024];
	const DWORD path_length = GetModuleFileNameA(NULL, path, sizeof(path));
	TEST_CHECK(path_length > 0 && path_length < sizeof(path));
	start = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start != NULL);
	struct SpawnState spawn = {start, path, 0};
	HANDLE spawner = CreateThread(NULL, 0, spawn_worker, &spawn, 0, NULL);
	TEST_CHECK(spawner != NULL);
	const BOOL spawn_started = SetEvent(start);
	BOOL spawn_error_preserved = TRUE;
	for (unsigned iteration = 0; iteration < concurrent_iterations; ++iteration) {
		SetLastError(0x52360001u);
		flush_write_buffers();
		if (GetLastError() != 0x52360001u)
			spawn_error_preserved = FALSE;
	}
	const DWORD spawn_result = join_worker(spawner, spawn_join_ms);
	TEST_CHECK_EQ(0, spawn_result);
	TEST_CHECK(spawn_started);
	TEST_CHECK(spawn_error_preserved);
	TEST_CHECK_EQ(spawn_iterations, spawn.completed);
	TEST_CHECK(CloseHandle(spawner));
	TEST_CHECK(CloseHandle(start));
	printf("visibility iterations=%u forbidden=%u concurrent calls=%u children=%u\n", completed, forbidden,
		   concurrent_iterations * concurrent_workers, spawn.completed);
	return 0;
}
