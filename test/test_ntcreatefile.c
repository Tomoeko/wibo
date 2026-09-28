#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <windows.h>
#include <winternl.h>

#ifndef FILE_OPENED
#define FILE_OPENED 1
#endif

typedef NTSTATUS(NTAPI *NtCreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
										ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *NtCloseFn)(HANDLE);
typedef NTSTATUS(NTAPI *NtQueryDirectoryFileFn)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
												FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
typedef BOOLEAN(NTAPI *RtlCreateUnicodeStringFn)(PUNICODE_STRING, LPCWSTR);
typedef VOID(NTAPI *RtlFreeUnicodeStringFn)(PUNICODE_STRING);

static void *load_function(HMODULE module, const char *name) {
	FARPROC address = GetProcAddress(module, name);
	TEST_CHECK_MSG(address != NULL, "Missing %s", name);
	return (void *)(uintptr_t)address;
}

static UNICODE_STRING counted_string(const WCHAR *text) {
	UNICODE_STRING name;
	name.Length = (USHORT)(lstrlenW(text) * sizeof(WCHAR));
	name.MaximumLength = name.Length + sizeof(WCHAR);
	name.Buffer = (PWSTR)text;
	return name;
}

static void append_wide(WCHAR *destination, const WCHAR *source) {
	DWORD used = lstrlenW(destination);
	DWORD length = lstrlenW(source);
	memcpy(destination + used, source, (length + 1) * sizeof(WCHAR));
}

static OBJECT_ATTRIBUTES object_attributes(UNICODE_STRING *name, HANDLE root) {
	OBJECT_ATTRIBUTES attributes;
	memset(&attributes, 0, sizeof(attributes));
	attributes.Length = sizeof(attributes);
	attributes.RootDirectory = root;
	attributes.ObjectName = name;
	attributes.Attributes = OBJ_CASE_INSENSITIVE;
	return attributes;
}

int main(void) {
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(ntdll != NULL);
	NtCreateFileFn create_file = (NtCreateFileFn)load_function(ntdll, "NtCreateFile");
	NtCloseFn close_file = (NtCloseFn)load_function(ntdll, "NtClose");
	NtQueryDirectoryFileFn query_directory = (NtQueryDirectoryFileFn)load_function(ntdll, "NtQueryDirectoryFile");
	RtlCreateUnicodeStringFn create_string = (RtlCreateUnicodeStringFn)load_function(ntdll, "RtlCreateUnicodeString");
	RtlFreeUnicodeStringFn free_string = (RtlFreeUnicodeStringFn)load_function(ntdll, "RtlFreeUnicodeString");

	const WCHAR *directory_name = L"ntcreatefile_fixture";
	CreateDirectoryW(directory_name, NULL);
	DWORD directory_attributes = GetFileAttributesW(directory_name);
	TEST_CHECK(directory_attributes != INVALID_FILE_ATTRIBUTES && (directory_attributes & FILE_ATTRIBUTE_DIRECTORY));
	WCHAR child_path[MAX_PATH];
	child_path[0] = 0;
	append_wide(child_path, directory_name);
	append_wide(child_path, L"\\item.txt");
	HANDLE child = CreateFileW(child_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(child != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	TEST_CHECK(WriteFile(child, "sample", 6, &written, NULL));
	TEST_CHECK_EQ(6, written);
	TEST_CHECK(CloseHandle(child));
	WCHAR nested_path[MAX_PATH], nested_child_path[MAX_PATH];
	nested_path[0] = 0;
	append_wide(nested_path, directory_name);
	append_wide(nested_path, L"\\nested");
	TEST_CHECK(CreateDirectoryW(nested_path, NULL));
	nested_child_path[0] = 0;
	append_wide(nested_child_path, nested_path);
	append_wide(nested_child_path, L"\\inside.txt");
	child = CreateFileW(nested_child_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
						CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(child != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(child));
	WCHAR full_path[2048], native_path[2052];
	DWORD full_length = GetFullPathNameW(directory_name, 2048, full_path, NULL);
	TEST_CHECK(full_length > 0 && full_length < 2048);
	native_path[0] = 0;
	append_wide(native_path, L"\\??\\");
	append_wide(native_path, full_path);
	UNICODE_STRING directory_string = counted_string(native_path);
	OBJECT_ATTRIBUTES directory_attrs = object_attributes(&directory_string, NULL);
	IO_STATUS_BLOCK iosb;
	memset(&iosb, 0, sizeof(iosb));
	HANDLE directory = NULL;
	SetLastError(ERROR_GEN_FAILURE);
	NTSTATUS status =
		create_file(&directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &directory_attrs, &iosb, NULL,
					FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
					FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	TEST_CHECK_EQ(0, status);
	TEST_CHECK_EQ(0, iosb.Status);
	TEST_CHECK_EQ(FILE_OPENED, iosb.Information);
	TEST_CHECK_EQ(ERROR_GEN_FAILURE, GetLastError());
	TEST_CHECK(directory != NULL && directory != INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(FILE_TYPE_DISK, GetFileType(directory));

	UNICODE_STRING relative_name = counted_string(L"item.txt");
	OBJECT_ATTRIBUTES relative_attrs = object_attributes(&relative_name, directory);
	HANDLE relative_file = NULL;
	memset(&iosb, 0, sizeof(iosb));
	status = create_file(&relative_file, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &relative_attrs, &iosb,
						 NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
						 FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	TEST_CHECK_EQ(0, status);
	TEST_CHECK_EQ(FILE_OPENED, iosb.Information);
	char contents[8] = {0};
	DWORD read = 0;
	TEST_CHECK(ReadFile(relative_file, contents, 6, &read, NULL));
	TEST_CHECK_EQ(6, read);
	TEST_CHECK(memcmp(contents, "sample", 6) == 0);
	TEST_CHECK_EQ(0, close_file(relative_file));
	UNICODE_STRING nested_name = counted_string(L"nested");
	OBJECT_ATTRIBUTES nested_attrs = object_attributes(&nested_name, directory);
	HANDLE nested_directory = NULL;
	memset(&iosb, 0, sizeof(iosb));
	status = create_file(&nested_directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &nested_attrs,
						 &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						 FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	TEST_CHECK_EQ(0, status);
	TEST_CHECK_EQ(FILE_OPENED, iosb.Information);
	TEST_CHECK_EQ(FILE_TYPE_DISK, GetFileType(nested_directory));
	WCHAR nested_information[4096];
	memset(&iosb, 0, sizeof(iosb));
	status = query_directory(nested_directory, NULL, NULL, NULL, &iosb, nested_information, sizeof(nested_information),
							 FileDirectoryInformation, FALSE, NULL, TRUE);
	TEST_CHECK_EQ(0, status);
	TEST_CHECK(iosb.Information > 0);
	TEST_CHECK_EQ(0, close_file(nested_directory));

	WCHAR information[4096];
	memset(&iosb, 0, sizeof(iosb));
	status = query_directory(directory, NULL, NULL, NULL, &iosb, information, sizeof(information),
							 FileDirectoryInformation, FALSE, NULL, TRUE);
	TEST_CHECK_EQ(0, status);
	TEST_CHECK(iosb.Information > 0);

	UNICODE_STRING missing_name = counted_string(L"absent.txt");
	OBJECT_ATTRIBUTES missing_attrs = object_attributes(&missing_name, directory);
	HANDLE missing_file = NULL;
	memset(&iosb, 0, sizeof(iosb));
	status =
		create_file(&missing_file, FILE_READ_DATA | SYNCHRONIZE, &missing_attrs, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
					FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	TEST_CHECK(status < 0);
	TEST_CHECK(missing_file == NULL);

	UNICODE_STRING allocated = {0};
	TEST_CHECK(create_string(&allocated, L"allocation check"));
	TEST_CHECK_EQ(16 * sizeof(WCHAR), allocated.Length);
	TEST_CHECK_EQ(17 * sizeof(WCHAR), allocated.MaximumLength);
	TEST_CHECK(lstrcmpW(allocated.Buffer, L"allocation check") == 0);
	free_string(&allocated);
	TEST_CHECK(allocated.Buffer == NULL);
	TEST_CHECK_EQ(0, allocated.Length);
	TEST_CHECK_EQ(0, allocated.MaximumLength);

	TEST_CHECK_EQ(0, close_file(directory));
	TEST_CHECK(DeleteFileW(child_path));
	TEST_CHECK(DeleteFileW(nested_child_path));
	TEST_CHECK(RemoveDirectoryW(nested_path));
	TEST_CHECK(RemoveDirectoryW(directory_name));
	return 0;
}
