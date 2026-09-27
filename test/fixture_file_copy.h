#ifndef FIXTURE_FILE_COPY_H
#define FIXTURE_FILE_COPY_H

#include <windows.h>

// A failed copy still belongs to the caller once CREATE_NEW succeeds.
static int copyOwnedFixtureFile(const char *source, const char *destination, int *created) {
	int result = 0;
	HANDLE input = CreateFileA(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE output = INVALID_HANDLE_VALUE;
	unsigned char bytes[4096];
	DWORD total = 0;
	*created = 0;
	if (input == INVALID_HANDLE_VALUE)
		goto cleanup;
	output = CreateFileA(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE)
		goto cleanup;
	*created = 1;
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(input, bytes, sizeof(bytes), &read, NULL))
			goto cleanup;
		if (!read)
			break;
		if (read > sizeof(bytes) || read > 1024 * 1024 - total)
			goto cleanup;
		for (DWORD offset = 0; offset < read;) {
			DWORD written = 0;
			if (!WriteFile(output, bytes + offset, read - offset, &written, NULL) || !written ||
				written > read - offset)
				goto cleanup;
			offset += written;
		}
		total += read;
	}
	result = total != 0;
cleanup:
	if (output != INVALID_HANDLE_VALUE && !CloseHandle(output))
		result = 0;
	if (input != INVALID_HANDLE_VALUE && !CloseHandle(input))
		result = 0;
	return result;
}

#endif
