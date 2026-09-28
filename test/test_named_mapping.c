#include <windows.h>

#include "test_assert.h"

static const WCHAR kMappingName[] = L"Local\\wibo_synthetic_named_mapping_90c4f622";
static const char kMappingNameA[] = "Local\\wibo_synthetic_named_mapping_90c4f622";
static const WCHAR kMissingName[] = L"Local\\wibo_synthetic_named_mapping_90c4f622_missing";
static const WCHAR kCollisionName[] = L"Local\\wibo_synthetic_named_mapping_90c4f622_event";
static const WCHAR kReadOnlyName[] = L"Local\\wibo_synthetic_named_mapping_90c4f622_readonly";
static const WCHAR kViewLifetimeName[] = L"Local\\wibo_synthetic_named_mapping_90c4f622_view";

static void test_named_mapping(void) {
	SetLastError(0x12345678);
	TEST_CHECK(OpenFileMappingW(FILE_MAP_READ, FALSE, kMissingName) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());

	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, kMappingName);
	TEST_CHECK_MSG(mapping != NULL, "CreateFileMappingW failed: %lu", GetLastError());
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	BYTE *writer = (BYTE *)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096);
	TEST_CHECK_MSG(writer != NULL, "writer view failed: %lu", GetLastError());
	writer[0] = 0x35;

	HANDLE reader = OpenFileMappingW(FILE_MAP_READ, TRUE, kMappingName);
	TEST_CHECK_MSG(reader != NULL, "OpenFileMappingW failed: %lu", GetLastError());
	DWORD flags = 0;
	TEST_CHECK(GetHandleInformation(reader, &flags));
	TEST_CHECK((flags & HANDLE_FLAG_INHERIT) != 0);
	BYTE *read_view = (BYTE *)MapViewOfFile(reader, FILE_MAP_READ, 0, 0, 4096);
	TEST_CHECK_MSG(read_view != NULL, "reader view failed: %lu", GetLastError());
	TEST_CHECK_EQ(0x35, read_view[0]);
	SetLastError(0x12345678);
	TEST_CHECK(MapViewOfFile(reader, FILE_MAP_WRITE, 0, 0, 4096) == NULL);
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());

	HANDLE copy_handle = OpenFileMappingW(FILE_MAP_COPY, FALSE, kMappingName);
	TEST_CHECK_MSG(copy_handle != NULL, "OpenFileMappingW(FILE_MAP_COPY) failed: %lu", GetLastError());
	BYTE *copy = (BYTE *)MapViewOfFile(copy_handle, FILE_MAP_COPY, 0, 0, 4096);
	TEST_CHECK_MSG(copy != NULL, "copy view failed: %lu", GetLastError());
	TEST_CHECK_EQ(0x35, copy[0]);
	copy[0] = 0x92;
	TEST_CHECK_EQ(0x35, writer[0]);

	HANDLE existing = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READONLY, 0, 4096, kMappingNameA);
	TEST_CHECK_MSG(existing != NULL, "CreateFileMappingA reopen failed: %lu", GetLastError());
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	BYTE *existing_view = (BYTE *)MapViewOfFile(existing, FILE_MAP_READ, 0, 0, 4096);
	TEST_CHECK_MSG(existing_view != NULL, "existing view failed: %lu", GetLastError());
	TEST_CHECK_EQ(0x35, existing_view[0]);
	SetLastError(0x12345678);
	TEST_CHECK(MapViewOfFile(existing, FILE_MAP_WRITE, 0, 0, 4096) == NULL);
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());

	TEST_CHECK(UnmapViewOfFile(existing_view));
	TEST_CHECK(UnmapViewOfFile(copy));
	TEST_CHECK(UnmapViewOfFile(read_view));
	TEST_CHECK(UnmapViewOfFile(writer));
	TEST_CHECK(CloseHandle(existing));
	TEST_CHECK(CloseHandle(copy_handle));
	TEST_CHECK(CloseHandle(reader));
	TEST_CHECK(CloseHandle(mapping));
	SetLastError(0x12345678);
	TEST_CHECK(OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
}

static void test_namespace_collision(void) {
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, kCollisionName);
	TEST_CHECK(event != NULL);
	SetLastError(0x12345678);
	TEST_CHECK(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, kCollisionName) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(event));
}

static void test_readonly_open_access(void) {
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READONLY, 0, 4096, kReadOnlyName);
	TEST_CHECK(mapping != NULL);
	HANDLE writer = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReadOnlyName);
	TEST_CHECK(writer != NULL);
	TEST_CHECK(CloseHandle(writer));
	TEST_CHECK(CloseHandle(mapping));
}

static void test_unnamed_views_share_pages(void) {
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
	TEST_CHECK(mapping != NULL);
	BYTE *first = (BYTE *)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096);
	BYTE *second = (BYTE *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096);
	TEST_CHECK(first != NULL && second != NULL);
	first[0] = 0x71;
	TEST_CHECK_EQ(0x71, second[0]);
	TEST_CHECK(UnmapViewOfFile(second));
	TEST_CHECK(UnmapViewOfFile(first));
	TEST_CHECK(CloseHandle(mapping));
}

static void test_view_keeps_name(void) {
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, kViewLifetimeName);
	TEST_CHECK(mapping != NULL);
	BYTE *view = (BYTE *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096);
	TEST_CHECK(view != NULL);
	TEST_CHECK(CloseHandle(mapping));
	SetLastError(0x12345678);
	HANDLE reopened = OpenFileMappingW(FILE_MAP_READ, FALSE, kViewLifetimeName);
	TEST_CHECK(reopened == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK(UnmapViewOfFile(view));
}

int main(void) {
	test_named_mapping();
	test_namespace_collision();
	test_readonly_open_access();
	test_unnamed_views_share_pages();
	test_view_keeps_name();
	return 0;
}
