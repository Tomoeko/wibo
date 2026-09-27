#include "test_assert.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static unsigned char bytes[0x800];
static HANDLE image_file(unsigned bits, const char *name) {
	memset(bytes, 0, sizeof(bytes));
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)bytes;
	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = 0x80;
	DWORD *signature = (DWORD *)(bytes + 0x80);
	*signature = IMAGE_NT_SIGNATURE;
	IMAGE_FILE_HEADER *file = (IMAGE_FILE_HEADER *)(signature + 1);
	file->Machine = bits == 64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
	file->NumberOfSections = 2;
	file->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL | IMAGE_FILE_RELOCS_STRIPPED;
	IMAGE_SECTION_HEADER *section;
	if (bits == 64) {
		IMAGE_OPTIONAL_HEADER64 *optional = (IMAGE_OPTIONAL_HEADER64 *)(file + 1);
		file->SizeOfOptionalHeader = sizeof(*optional);
		optional->Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
		optional->ImageBase = 0x64000000;
		optional->SectionAlignment = 0x1000;
		optional->FileAlignment = 0x200;
		optional->SizeOfImage = 0x3000;
		optional->SizeOfHeaders = 0x400;
		optional->MajorOperatingSystemVersion = 6;
		optional->MajorSubsystemVersion = 6;
		optional->Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
		optional->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
		optional->SizeOfStackReserve = 0x100000;
		optional->SizeOfStackCommit = 0x1000;
		section = (IMAGE_SECTION_HEADER *)(optional + 1);
	} else {
		IMAGE_OPTIONAL_HEADER32 *optional = (IMAGE_OPTIONAL_HEADER32 *)(file + 1);
		file->SizeOfOptionalHeader = sizeof(*optional);
		optional->Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
		optional->ImageBase = 0x64000000;
		optional->SectionAlignment = 0x1000;
		optional->FileAlignment = 0x200;
		optional->SizeOfImage = 0x3000;
		optional->SizeOfHeaders = 0x400;
		optional->MajorOperatingSystemVersion = 6;
		optional->MajorSubsystemVersion = 6;
		optional->Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
		optional->NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
		optional->SizeOfStackReserve = 0x100000;
		optional->SizeOfStackCommit = 0x1000;
		section = (IMAGE_SECTION_HEADER *)(optional + 1);
	}
	memcpy(section[0].Name, ".text", 5);
	section[0].VirtualAddress = 0x1000;
	section[0].Misc.VirtualSize = 0x1000;
	section[0].SizeOfRawData = 0x200;
	section[0].PointerToRawData = 0x400;
	section[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
	memcpy(section[1].Name, ".data", 5);
	section[1].VirtualAddress = 0x2000;
	section[1].Misc.VirtualSize = 0x1000;
	section[1].SizeOfRawData = 0x200;
	section[1].PointerToRawData = 0x600;
	section[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
	bytes[0x400] = 0x23;
	bytes[0x600] = 0x37;
	*(uint64_t *)(bytes + 0x608) = 0x64002000;
	HANDLE handle =
		CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
					CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return handle;
	DWORD written;
	if (!WriteFile(handle, bytes, sizeof(bytes), &written, NULL) || written != sizeof(bytes)) {
		CloseHandle(handle);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

static void check_region(unsigned char *base, size_t offset, DWORD protect) {
	MEMORY_BASIC_INFORMATION info = {0};
	TEST_CHECK_EQ(sizeof(info), VirtualQuery(base + offset, &info, sizeof(info)));
	TEST_CHECK(info.AllocationBase == base);
	TEST_CHECK(info.BaseAddress == base + offset);
	TEST_CHECK_EQ(0x1000, info.RegionSize);
	TEST_CHECK_EQ(MEM_COMMIT, info.State);
	TEST_CHECK_EQ(MEM_IMAGE, info.Type);
	TEST_CHECK_EQ(PAGE_EXECUTE_WRITECOPY, info.AllocationProtect);
	TEST_CHECK_EQ(protect, info.Protect);
}

static void check_image(unsigned bits) {
	HANDLE file = image_file(bits, "test_image_mapping.tmp");
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK_EQ(0x17, SetFilePointer(file, 0x17, NULL, FILE_BEGIN));
	HANDLE mapping = CreateFileMappingW(file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL);
	TEST_CHECK_MSG(mapping != NULL, "image mapping failed: %lu", GetLastError());
	TEST_CHECK_EQ(0x17, SetFilePointer(file, 0, NULL, FILE_CURRENT));
	unsigned char *anchor = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	TEST_CHECK_MSG(anchor != NULL, "initial image view failed: %lu", GetLastError());
	check_region(anchor, 0x2000, PAGE_WRITECOPY);
	DWORD accesses[] = {0, FILE_MAP_READ, FILE_MAP_COPY, FILE_MAP_EXECUTE};
	for (unsigned i = 0; i < sizeof(accesses) / sizeof(accesses[0]); ++i) {
		unsigned char *view = MapViewOfFile(mapping, accesses[i], 0, 0, 1);
		TEST_CHECK_MSG(view != NULL, "image view failed: %lu", GetLastError());
		TEST_CHECK_EQ(0x17, SetFilePointer(file, 0, NULL, FILE_CURRENT));
		TEST_CHECK(view != anchor);
		check_region(view, 0, PAGE_READONLY);
		check_region(view, 0x1000, PAGE_EXECUTE_READ);
		check_region(view, 0x2000, PAGE_WRITECOPY);
		TEST_CHECK_EQ(0x23, view[0x1000]);
		TEST_CHECK_EQ(0x37, view[0x2000]);
		TEST_CHECK_EQ(0, view[0x2200]);
		TEST_CHECK_U64_EQ(0x64002000, *(uint64_t *)(view + 0x2008));
		TEST_CHECK(UnmapViewOfFile(view));
	}
	TEST_CHECK(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) == NULL);
	TEST_CHECK(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0x10000, 0) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	unsigned char *first = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0x4000);
	unsigned char *second = MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, 0);
	TEST_CHECK(first != NULL && second != NULL && first != second);
	check_region(first, 0x2000, PAGE_WRITECOPY);
	TEST_CHECK(!VirtualFree(first, 0, MEM_RELEASE));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	DWORD old_protect;
	TEST_CHECK(VirtualProtect(first + 0x2000, 0x1000, PAGE_READONLY, &old_protect));
	TEST_CHECK_EQ(PAGE_WRITECOPY, old_protect);
	check_region(first, 0x2000, PAGE_READONLY);
	TEST_CHECK(VirtualProtect(first + 0x2000, 0x1000, PAGE_WRITECOPY, &old_protect));
	TEST_CHECK_EQ(PAGE_READONLY, old_protect);
	first[0x2000] = 0xa5;
	TEST_CHECK_EQ(0x37, second[0x2000]);
	TEST_CHECK(CloseHandle(mapping));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK_EQ(0xa5, first[0x2000]);
	TEST_CHECK_EQ(0x37, second[0x2000]);
	TEST_CHECK_EQ(0x37, anchor[0x2000]);
	TEST_CHECK(UnmapViewOfFile(first + 0x1000));
	TEST_CHECK(UnmapViewOfFile(second));
	TEST_CHECK(UnmapViewOfFile(anchor));
}

static void check_create_failures(void) {
	HANDLE file = image_file(64, "test_image_mapping.tmp");
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_NOACCESS, 0, 0, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_READONLY | SEC_COMMIT, 0, 0, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0x3000, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_BAD_EXE_FORMAT, GetLastError());
	HANDLE mapping = CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 1, NULL);
	TEST_CHECK(mapping != NULL);
	TEST_CHECK(CloseHandle(mapping));
	TEST_CHECK(CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0x4000, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_ENOUGH_MEMORY, GetLastError());
	memset(bytes, 0, sizeof(bytes));
	DWORD written = 0;
	TEST_CHECK(SetFilePointer(file, 0, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
	TEST_CHECK(WriteFile(file, bytes, sizeof(bytes), &written, NULL));
	TEST_CHECK_EQ(sizeof(bytes), written);
	TEST_CHECK(CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_BAD_EXE_FORMAT, GetLastError());
	TEST_CHECK(CloseHandle(file));
}

int main(void) {
	check_image(32);
	check_image(64);
	check_create_failures();
	return EXIT_SUCCESS;
}
