#include "common.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <variant>

constexpr uint16_t IMAGE_FILE_DLL = 0x2000;

struct PEHeader {
	uint8_t magic[4]; // "PE\0\0"
	uint16_t machine;
	uint16_t numberOfSections;
	uint32_t timeDateStamp;
	uint32_t pointerToSymbolTable;
	uint32_t numberOfSymbols;
	uint16_t sizeOfOptionalHeader;
	uint16_t characteristics;
};
struct PEImageDataDirectory {
	uint32_t virtualAddress;
	uint32_t size;
};
struct PE32Header {
	uint16_t magic; // 0x10B for PE32
	uint8_t majorLinkerVersion;
	uint8_t minorLinkerVersion;
	uint32_t sizeOfCode;
	uint32_t sizeOfInitializedData;
	uint32_t sizeOfUninitializedData;
	uint32_t addressOfEntryPoint;
	uint32_t baseOfCode;
	uint32_t baseOfData;
	uint32_t imageBase;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint16_t majorOperatingSystemVersion;
	uint16_t minorOperatingSystemVersion;
	uint16_t majorImageVersion;
	uint16_t minorImageVersion;
	uint16_t majorSubsystemVersion;
	uint16_t minorSubsystemVersion;
	uint32_t win32VersionValue;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	uint32_t checkSum;
	uint16_t subsystem;
	uint16_t dllCharacteristics;
	uint32_t sizeOfStackReserve;
	uint32_t sizeOfStackCommit;
	uint32_t sizeOfHeapReserve;
	uint32_t sizeOfHeapCommit;
	uint32_t loaderFlags;
	uint32_t numberOfRvaAndSizes;
	PEImageDataDirectory exportTable;
	PEImageDataDirectory importTable;	// *
	PEImageDataDirectory resourceTable; // *
	PEImageDataDirectory exceptionTable;
	PEImageDataDirectory certificateTable;
	PEImageDataDirectory baseRelocationTable; // *
	PEImageDataDirectory debug;				  // *
	PEImageDataDirectory architecture;
	PEImageDataDirectory globalPtr;
	PEImageDataDirectory tlsTable;
	PEImageDataDirectory loadConfigTable;
	PEImageDataDirectory boundImport;
	PEImageDataDirectory iat;
	PEImageDataDirectory delayImportDescriptor;
	PEImageDataDirectory clrRuntimeHeader;
	PEImageDataDirectory reserved;
};
struct PE64Header {
	uint16_t magic; // 0x20B for PE32+
	uint8_t majorLinkerVersion;
	uint8_t minorLinkerVersion;
	uint32_t sizeOfCode;
	uint32_t sizeOfInitializedData;
	uint32_t sizeOfUninitializedData;
	uint32_t addressOfEntryPoint;
	uint32_t baseOfCode;
	uint64_t imageBase;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint16_t majorOperatingSystemVersion;
	uint16_t minorOperatingSystemVersion;
	uint16_t majorImageVersion;
	uint16_t minorImageVersion;
	uint16_t majorSubsystemVersion;
	uint16_t minorSubsystemVersion;
	uint32_t win32VersionValue;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	uint32_t checkSum;
	uint16_t subsystem;
	uint16_t dllCharacteristics;
	uint64_t sizeOfStackReserve;
	uint64_t sizeOfStackCommit;
	uint64_t sizeOfHeapReserve;
	uint64_t sizeOfHeapCommit;
	uint32_t loaderFlags;
	uint32_t numberOfRvaAndSizes;
	PEImageDataDirectory exportTable;
	PEImageDataDirectory importTable;
	PEImageDataDirectory resourceTable;
	PEImageDataDirectory exceptionTable;
	PEImageDataDirectory certificateTable;
	PEImageDataDirectory baseRelocationTable;
	PEImageDataDirectory debug;
	PEImageDataDirectory architecture;
	PEImageDataDirectory globalPtr;
	PEImageDataDirectory tlsTable;
	PEImageDataDirectory loadConfigTable;
	PEImageDataDirectory boundImport;
	PEImageDataDirectory iat;
	PEImageDataDirectory delayImportDescriptor;
	PEImageDataDirectory clrRuntimeHeader;
	PEImageDataDirectory reserved;
};
#ifdef WIBO_GUEST_64
constexpr uint16_t kExpectedMachine = 0x8664;
#else
constexpr uint16_t kExpectedMachine = 0x14C;
#endif
struct PESectionHeader {
	char name[8];
	uint32_t virtualSize;
	uint32_t virtualAddress;
	uint32_t sizeOfRawData;
	uint32_t pointerToRawData;
	uint32_t pointerToRelocations;
	uint32_t pointerToLinenumbers;
	uint16_t numberOfRelocations;
	uint16_t numberOfLinenumbers;
	uint32_t characteristics;
};
struct PEImportDirectoryEntry {
	uint32_t importLookupTable;
	uint32_t timeDateStamp;
	uint32_t forwarderChain;
	uint32_t name;
	uint32_t importAddressTable;
};
struct PEHintNameTableEntry {
	uint16_t hint;
	char name[1]; // variable length
};

struct PEDelayImportDescriptor {
	uint32_t attributes;
	uint32_t name;
	uint32_t moduleHandle;
	uint32_t importAddressTable;
	uint32_t importNameTable;
	uint32_t boundImportAddressTable;
	uint32_t unloadInformationTable;
	uint32_t timeStamp;
};

struct PEBaseRelocationBlock {
	uint32_t virtualAddress;
	uint32_t sizeOfBlock;
};

constexpr uint16_t IMAGE_REL_BASED_ABSOLUTE = 0;
constexpr uint16_t IMAGE_REL_BASED_HIGHLOW = 3;
constexpr uint16_t IMAGE_REL_BASED_DIR64 = 10;

constexpr uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
constexpr uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;
constexpr uint32_t IMAGE_SCN_MEM_SHARED = 0x10000000;
constexpr uint32_t IMAGE_SCN_MEM_NOT_CACHED = 0x04000000;

static uintptr_t alignDown(uintptr_t value, size_t alignment) {
	if (alignment == 0) {
		return value;
	}
	return value - (value % alignment);
}

static uintptr_t alignUp(uintptr_t value, size_t alignment) {
	if (alignment == 0) {
		return value;
	}
	const uintptr_t remainder = value % alignment;
	if (remainder == 0) {
		return value;
	}
	if (value > std::numeric_limits<uintptr_t>::max() - (alignment - remainder)) {
		return std::numeric_limits<uintptr_t>::max();
	}
	return value + (alignment - remainder);
}

static DWORD sectionProtectFromCharacteristics(uint32_t characteristics, bool dataImage = false) {
	const bool executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
	bool readable = (characteristics & IMAGE_SCN_MEM_READ) != 0;
	const bool writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
	if (!dataImage && !readable && !writable && !executable) {
		readable = true;
	}
	DWORD protect = PAGE_NOACCESS;
	if (executable) {
		if (writable) {
			protect = PAGE_EXECUTE_READWRITE;
		} else if (readable) {
			protect = PAGE_EXECUTE_READ;
		} else {
			protect = PAGE_EXECUTE;
		}
	} else {
		if (writable) {
			protect = PAGE_READWRITE;
		} else if (readable) {
			protect = PAGE_READONLY;
		}
	}
	if ((characteristics & IMAGE_SCN_MEM_NOT_CACHED) != 0) {
		protect |= PAGE_NOCACHE;
	}
	return protect;
}

wibo::Executable::~Executable() {
	if (imageBase) {
		wibo::heap::virtualFree(imageBase, 0, MEM_RELEASE);
		imageBase = nullptr;
	}
}

namespace {

struct ImageMemoryDeleter {
	wibo::Executable *owner = nullptr;
	void operator()(void *ptr) const {
		if (ptr) {
			wibo::heap::virtualFree(ptr, 0, MEM_RELEASE);
		}
		if (owner && owner->imageBase == ptr) {
			owner->imageBase = nullptr;
			owner->imageSize = 0;
			owner->entryPoint = nullptr;
			owner->rsrcBase = nullptr;
			owner->rsrcSize = 0;
			owner->sections.clear();
		}
	}
};

class PeInputView {
  public:
	explicit PeInputView(FILE *file) : source_(FileSource{file, computeFileSize(file)}) {}
	explicit PeInputView(std::span<const uint8_t> bytes) : source_(SpanSource{bytes}) {}

	bool read(uint64_t offset, void *dest, size_t size) const {
		if (size == 0) {
			return true;
		}
		return std::visit([&](const auto &source) { return readImpl(source, offset, dest, size); }, source_);
	}

	template <typename T> std::optional<T> readObject(uint64_t offset) const {
		T value{};
		if (!read(offset, &value, sizeof(T))) {
			return std::nullopt;
		}
		return value;
	}

	std::optional<uint64_t> size() const {
		return std::visit([](const auto &source) -> std::optional<uint64_t> { return sizeImpl(source); }, source_);
	}

  private:
	struct FileSource {
		FILE *file;
		std::optional<uint64_t> fileSize;
	};
	struct SpanSource {
		std::span<const uint8_t> bytes;
	};

	static std::optional<uint64_t> computeFileSize(FILE *file) {
		if (!file) {
			return std::nullopt;
		}
		int fd = fileno(file);
		if (fd < 0) {
			return std::nullopt;
		}
		struct stat st {};
		if (fstat(fd, &st) != 0) {
			return std::nullopt;
		}
		if (st.st_size < 0) {
			return std::nullopt;
		}
		return static_cast<uint64_t>(st.st_size);
	}

	static bool readImpl(const FileSource &source, uint64_t offset, void *dest, size_t size) {
		if (!source.file) {
			return false;
		}
		if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
			return false;
		}
		if (source.fileSize) {
			if (offset > *source.fileSize) {
				return false;
			}
			uint64_t remaining = *source.fileSize - offset;
			if (remaining < size) {
				return false;
			}
		}
		unsigned char *buffer = static_cast<unsigned char *>(dest);
		size_t totalRead = 0;
		while (totalRead < size) {
			if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - totalRead) {
				return false;
			}
			ssize_t chunk = pread(fileno(source.file), buffer + totalRead, size - totalRead,
								  static_cast<off_t>(offset + totalRead));
			if (chunk < 0 && errno == EINTR) {
				continue;
			}
			if (chunk <= 0) {
				return false;
			}
			totalRead += static_cast<size_t>(chunk);
		}
		return totalRead == size;
	}

	static bool readImpl(const SpanSource &source, uint64_t offset, void *dest, size_t size) {
		if (offset > source.bytes.size()) {
			return false;
		}
		size_t start = static_cast<size_t>(offset);
		if (source.bytes.size() - start < size) {
			return false;
		}
		auto slice = source.bytes.subspan(start, size);
		std::memcpy(dest, slice.data(), size);
		return true;
	}

	static std::optional<uint64_t> sizeImpl(const FileSource &source) { return source.fileSize; }
	static std::optional<uint64_t> sizeImpl(const SpanSource &source) {
		return static_cast<uint64_t>(source.bytes.size());
	}

	std::variant<FileSource, SpanSource> source_;
};

void resetExecutableState(wibo::Executable &executable) {
	if (executable.imageBase) {
		wibo::heap::virtualFree(executable.imageBase, 0, MEM_RELEASE);
	}
	executable.imageBase = nullptr;
	executable.imageSize = 0;
	executable.entryPoint = nullptr;
	executable.subsystem = 0;
	executable.rsrcBase = nullptr;
	executable.rsrcSize = 0;
	executable.preferredImageBase = 0;
	executable.relocationDelta = 0;
	executable.stackReserveSize = 0;
	executable.stackCommitSize = 0;
	executable.exportDirectoryRVA = 0;
	executable.exportDirectorySize = 0;
	executable.relocationDirectoryRVA = 0;
	executable.relocationDirectorySize = 0;
	executable.importDirectoryRVA = 0;
	executable.importDirectorySize = 0;
	executable.delayImportDirectoryRVA = 0;
	executable.delayImportDirectorySize = 0;
	executable.tlsDirectoryRVA = 0;
	executable.tlsDirectorySize = 0;
	executable.exceptionDirectoryRVA = 0;
	executable.exceptionDirectorySize = 0;
	executable.execMapped = false;
	executable.importsResolved = false;
	executable.importsResolving = false;
	executable.sectionsProtected = false;
	executable.sections.clear();
}

struct ParsedPeImage {
	PEHeader fileHeader{};
	PE64Header optionalHeader{};
	std::vector<PESectionHeader> sections;
};

static_assert(sizeof(PE32Header) == 224);
static_assert(sizeof(PE64Header) == 240);
static_assert(offsetof(PE32Header, exportTable) == 96);
static_assert(offsetof(PE64Header, exportTable) == 112);

template <typename Header>
bool readOptionalHeader(const PeInputView &source, uint64_t offset, size_t size, PE64Header &normalized) {
	constexpr size_t directoryOffset = offsetof(Header, exportTable);
	if (size < directoryOffset) {
		return false;
	}
	Header header{};
	if (!source.read(offset, &header, std::min(sizeof(header), size))) {
		return false;
	}
	if (header.numberOfRvaAndSizes > (size - directoryOffset) / sizeof(PEImageDataDirectory)) {
		return false;
	}
	normalized.magic = header.magic;
	normalized.addressOfEntryPoint = header.addressOfEntryPoint;
	normalized.subsystem = header.subsystem;
	normalized.imageBase = header.imageBase;
	normalized.sectionAlignment = header.sectionAlignment;
	normalized.fileAlignment = header.fileAlignment;
	normalized.sizeOfImage = header.sizeOfImage;
	normalized.sizeOfHeaders = header.sizeOfHeaders;
	normalized.sizeOfStackReserve = header.sizeOfStackReserve;
	normalized.sizeOfStackCommit = header.sizeOfStackCommit;
	normalized.numberOfRvaAndSizes = header.numberOfRvaAndSizes;
	const size_t directories = std::min<size_t>(header.numberOfRvaAndSizes, 16);
	std::memcpy(&normalized.exportTable, &header.exportTable, directories * sizeof(PEImageDataDirectory));
	return true;
}

bool parsePeImage(const PeInputView &source, bool dataImage, ParsedPeImage &image) {
	kernel32::setLastError(ERROR_BAD_EXE_FORMAT);

	auto dosSignature = source.readObject<uint16_t>(0);
	if (!dosSignature || *dosSignature != 0x5A4D) {
		DEBUG_LOG("loadPE: missing MZ header signature\n");
		return false;
	}

	auto offsetToPeOpt = source.readObject<uint32_t>(0x3C);
	if (!offsetToPeOpt) {
		DEBUG_LOG("loadPE: failed to read e_lfanew\n");
		return false;
	}
	uint32_t offsetToPE = *offsetToPeOpt;

	if (auto totalSize = source.size()) {
		if (offsetToPE > *totalSize || (*totalSize - offsetToPE) < sizeof(PEHeader)) {
			DEBUG_LOG("loadPE: PE header offset outside data (offset=%u size=%llu)\n", offsetToPE,
					  static_cast<unsigned long long>(*totalSize));
			return false;
		}
	}

	PEHeader header{};
	if (!source.read(offsetToPE, &header, sizeof(header))) {
		DEBUG_LOG("loadPE: unable to read PE header\n");
		return false;
	}
	if (std::memcmp(header.magic, "PE\0\0", 4) != 0) {
		DEBUG_LOG("loadPE: invalid PE signature\n");
		return false;
	}
	if ((!dataImage && header.machine != kExpectedMachine) ||
		(dataImage && header.machine != 0x14C && header.machine != 0x8664)) {
		DEBUG_LOG("loadPE: unsupported machine 0x%x\n", header.machine);
		return false;
	}
	if (header.numberOfSections == 0 || header.numberOfSections > 1024) {
		DEBUG_LOG("loadPE: unreasonable section count %u\n", header.numberOfSections);
		return false;
	}
	const uint64_t optionalOffset = static_cast<uint64_t>(offsetToPE) + sizeof(header);
	auto magic = source.readObject<uint16_t>(optionalOffset);
	PE64Header &header32 = image.optionalHeader;
	bool optionalValid = false;
	if (magic && *magic == 0x10B && header.machine == 0x14C) {
		optionalValid = readOptionalHeader<PE32Header>(source, optionalOffset, header.sizeOfOptionalHeader, header32);
	} else if (magic && *magic == 0x20B && header.machine == 0x8664) {
		optionalValid = readOptionalHeader<PE64Header>(source, optionalOffset, header.sizeOfOptionalHeader, header32);
	}
	if (!optionalValid) {
		DEBUG_LOG("loadPE: invalid optional header\n");
		return false;
	}
	if (header32.sizeOfImage == 0 || header32.sizeOfHeaders == 0 || header32.sizeOfHeaders > header32.sizeOfImage) {
		DEBUG_LOG("loadPE: invalid image/header sizes (image=%u headers=%u)\n", header32.sizeOfImage,
				  header32.sizeOfHeaders);
		return false;
	}
	if (header32.fileAlignment == 0 || header32.sectionAlignment == 0) {
		DEBUG_LOG("loadPE: invalid alignment (file=%u section=%u)\n", header32.fileAlignment,
				  header32.sectionAlignment);
		return false;
	}
	const uint64_t sectionOffset = optionalOffset + header.sizeOfOptionalHeader;
	const uint64_t sectionBytes = static_cast<uint64_t>(header.numberOfSections) * sizeof(PESectionHeader);
	auto fileSize = source.size();
	if (!fileSize || header32.sizeOfHeaders > *fileSize || sectionOffset > header32.sizeOfHeaders ||
		sectionBytes > header32.sizeOfHeaders - sectionOffset) {
		return false;
	}
	image.sections.resize(header.numberOfSections);
	if (!source.read(sectionOffset, image.sections.data(), static_cast<size_t>(sectionBytes))) {
		return false;
	}
	const size_t pageSize = wibo::heap::systemPageSize();
	if (dataImage &&
		((header32.fileAlignment & (header32.fileAlignment - 1)) != 0 ||
		 (header32.sectionAlignment & (header32.sectionAlignment - 1)) != 0 || header32.fileAlignment < 512 ||
		 header32.fileAlignment > 65536 || header32.sectionAlignment < header32.fileAlignment)) {
		return false;
	}
	uint64_t previousEnd = alignUp(header32.sizeOfHeaders, pageSize);
	for (const auto &section : image.sections) {
		const uint64_t span = std::max(section.virtualSize, section.sizeOfRawData);
		if (section.virtualAddress > header32.sizeOfImage || span > header32.sizeOfImage - section.virtualAddress ||
			(section.sizeOfRawData && section.pointerToRawData &&
			 (section.pointerToRawData > *fileSize || section.sizeOfRawData > *fileSize - section.pointerToRawData))) {
			return false;
		}
		if (dataImage && span) {
			if (header32.sectionAlignment < pageSize || (section.characteristics & IMAGE_SCN_MEM_SHARED)) {
				kernel32::setLastError(ERROR_NOT_SUPPORTED);
				return false;
			}
			if (section.virtualAddress % header32.sectionAlignment != 0 ||
				(section.pointerToRawData && section.pointerToRawData % header32.fileAlignment != 0)) {
				return false;
			}
			if (alignDown(section.virtualAddress, pageSize) < previousEnd) {
				return false;
			}
			previousEnd = alignUp(section.virtualAddress + span, pageSize);
		}
	}
	image.fileHeader = header;
	return true;
}

bool loadPEFromSource(wibo::Executable &executable, const PeInputView &source, bool exec, bool dataImage = false,
					  void *requestedBase = nullptr) {
	resetExecutableState(executable);
	ParsedPeImage image;
	if (!parsePeImage(source, dataImage, image)) {
		return false;
	}
	const auto &header = image.fileHeader;
	const auto &header32 = image.optionalHeader;
	executable.isDll = !!(header.characteristics & IMAGE_FILE_DLL);
	executable.subsystem = header32.subsystem;

	DEBUG_LOG("Sections: %u / Size of optional header: %x\n", header.numberOfSections, header.sizeOfOptionalHeader);
	DEBUG_LOG("Image Base: %llx / Size: %x\n", static_cast<unsigned long long>(header32.imageBase),
			  header32.sizeOfImage);

	long pageSize = sysconf(_SC_PAGE_SIZE);
	const size_t pageSizeValue = pageSize > 0 ? static_cast<size_t>(pageSize) : static_cast<size_t>(4096);
	DEBUG_LOG("Page size: %x\n", static_cast<unsigned int>(pageSizeValue));

	executable.preferredImageBase =
		header32.imageBase <= std::numeric_limits<uintptr_t>::max() ? static_cast<uintptr_t>(header32.imageBase) : 0;
	executable.stackReserveSize = static_cast<size_t>(std::min<uint64_t>(
		static_cast<uint64_t>(header32.sizeOfStackReserve), std::numeric_limits<size_t>::max()));
	executable.stackCommitSize = static_cast<size_t>(std::min<uint64_t>(
		static_cast<uint64_t>(header32.sizeOfStackCommit), std::numeric_limits<size_t>::max()));
	executable.exportDirectoryRVA = header32.exportTable.virtualAddress;
	executable.exportDirectorySize = header32.exportTable.size;
	executable.relocationDirectoryRVA = header32.baseRelocationTable.virtualAddress;
	executable.relocationDirectorySize = header32.baseRelocationTable.size;
	executable.importDirectoryRVA = header32.importTable.virtualAddress;
	executable.importDirectorySize = header32.importTable.size;
	executable.delayImportDirectoryRVA = header32.delayImportDescriptor.virtualAddress;
	executable.delayImportDirectorySize = header32.delayImportDescriptor.size;
	executable.tlsDirectoryRVA = header32.tlsTable.virtualAddress;
	executable.tlsDirectorySize = header32.tlsTable.size;
	executable.exceptionDirectoryRVA = header32.exceptionTable.virtualAddress;
	executable.exceptionDirectorySize = header32.exceptionTable.size;
	executable.execMapped = exec;
	executable.importsResolved = false;
	executable.importsResolving = false;
	executable.sectionsProtected = false;

	executable.imageSize = header32.sizeOfImage;
	DWORD initialProtect = dataImage ? PAGE_EXECUTE_WRITECOPY : exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
	void *preferredBase = requestedBase ? requestedBase : reinterpret_cast<void *>(executable.preferredImageBase);
	if (requestedBase && reinterpret_cast<uintptr_t>(requestedBase) % (64 * 1024) != 0) {
		kernel32::setLastError(ERROR_INVALID_ADDRESS);
		return false;
	}
	void *allocatedBase = preferredBase;
	std::size_t allocationSize = static_cast<std::size_t>(header32.sizeOfImage);
	wibo::heap::VmStatus allocStatus =
		wibo::heap::virtualAlloc(&allocatedBase, &allocationSize, MEM_RESERVE | MEM_COMMIT, initialProtect, MEM_IMAGE);
	if (allocStatus != wibo::heap::VmStatus::Success && !requestedBase) {
		DEBUG_LOG("loadPE: preferred base allocation failed (status=%u), retrying anywhere\n",
				  static_cast<unsigned>(allocStatus));
		allocatedBase = nullptr;
		allocationSize = static_cast<std::size_t>(header32.sizeOfImage);
		allocStatus = wibo::heap::virtualAlloc(&allocatedBase, &allocationSize, MEM_RESERVE | MEM_COMMIT,
											   initialProtect, MEM_IMAGE);
	}
	if (allocStatus != wibo::heap::VmStatus::Success) {
		DEBUG_LOG("loadPE: mapping failed (status=%u)\n", static_cast<unsigned>(allocStatus));
		kernel32::setLastError(wibo::heap::win32ErrorFromVmStatus(allocStatus));
		return false;
	}
	DEBUG_LOG("loadPE: mapping succeeded (base=%p, size=%zu)\n", allocatedBase, allocationSize);

	std::unique_ptr<void, ImageMemoryDeleter> imageGuard(allocatedBase, ImageMemoryDeleter{&executable});
	executable.imageBase = allocatedBase;
	executable.relocationDelta = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(executable.imageBase) -
												   static_cast<uintptr_t>(header32.imageBase));
	std::memset(executable.imageBase, 0, header32.sizeOfImage);
	if (!source.read(0, executable.imageBase, header32.sizeOfHeaders)) {
		DEBUG_LOG("loadPE: headers exceed available image or source data\n");
		return false;
	}
	executable.sections.clear();

	uintptr_t imageBaseAddr = reinterpret_cast<uintptr_t>(executable.imageBase);
	uintptr_t headerSpan = alignUp(static_cast<uintptr_t>(header32.sizeOfHeaders), pageSizeValue);
	if (headerSpan != 0) {
		wibo::Executable::SectionInfo headerInfo{};
		headerInfo.base = imageBaseAddr;
		headerInfo.size = static_cast<size_t>(headerSpan);
		headerInfo.protect = PAGE_READONLY;
		executable.sections.push_back(headerInfo);
	}

	for (uint16_t i = 0; i < header.numberOfSections; ++i) {
		const auto &section = image.sections[i];

		char name[9];
		std::memcpy(name, section.name, 8);
		name[8] = '\0';
		DEBUG_LOG("Section %u: name=%s addr=%x size=%x (raw=%x) ptr=%x\n", i, name, section.virtualAddress,
				  section.virtualSize, section.sizeOfRawData, section.pointerToRawData);

		void *sectionBase = reinterpret_cast<void *>(imageBaseAddr + section.virtualAddress);
		if (section.pointerToRawData != 0 && section.sizeOfRawData != 0) {
			if (!source.read(section.pointerToRawData, sectionBase, section.sizeOfRawData)) {
				DEBUG_LOG("loadPE: failed to load section %s data\n", name);
				return false;
			}
		}

		if (std::strcmp(name, ".rsrc") == 0) {
			executable.rsrcBase = sectionBase;
			executable.rsrcSize = std::max(section.virtualSize, section.sizeOfRawData);
		}

		size_t sectionSpan = std::max(section.virtualSize, section.sizeOfRawData);
		if (sectionSpan != 0) {
			uintptr_t sectionStart =
				alignDown(imageBaseAddr + static_cast<uintptr_t>(section.virtualAddress), pageSizeValue);
			uintptr_t sectionEnd = alignUp(imageBaseAddr + static_cast<uintptr_t>(section.virtualAddress) +
											   static_cast<uintptr_t>(sectionSpan),
										   pageSizeValue);
			if (sectionEnd < sectionStart) {
				DEBUG_LOG("loadPE: invalid span for section %s\n", name);
				return false;
			}
			if (sectionEnd > sectionStart) {
				wibo::Executable::SectionInfo sectionInfo{};
				sectionInfo.base = sectionStart;
				sectionInfo.size = static_cast<size_t>(sectionEnd - sectionStart);
				sectionInfo.protect = sectionProtectFromCharacteristics(section.characteristics, dataImage);
				if (dataImage) {
					const DWORD modifiers = sectionInfo.protect & ~0xFFu;
					if ((sectionInfo.protect & 0xFF) == PAGE_READWRITE) {
						sectionInfo.protect = PAGE_WRITECOPY | modifiers;
					} else if ((sectionInfo.protect & 0xFF) == PAGE_EXECUTE_READWRITE) {
						sectionInfo.protect = PAGE_EXECUTE_WRITECOPY | modifiers;
					}
				}
				sectionInfo.characteristics = section.characteristics;
				executable.sections.push_back(sectionInfo);
			}
		}
	}

	std::sort(executable.sections.begin(), executable.sections.end(),
			  [](const wibo::Executable::SectionInfo &lhs, const wibo::Executable::SectionInfo &rhs) {
				  return lhs.base < rhs.base;
			  });

	if (exec && executable.relocationDelta != 0) {
		if (executable.relocationDirectoryRVA == 0 || executable.relocationDirectorySize == 0) {
			DEBUG_LOG("Relocation required but no relocation directory present\n");
			return false;
		}

		uint8_t *relocCursor = executable.fromRVA<uint8_t>(executable.relocationDirectoryRVA);
		uint8_t *relocEnd = relocCursor + executable.relocationDirectorySize;
		while (relocCursor < relocEnd) {
			auto *block = reinterpret_cast<PEBaseRelocationBlock *>(relocCursor);
			if (block->sizeOfBlock < sizeof(PEBaseRelocationBlock) ||
				block->sizeOfBlock > static_cast<uint32_t>(relocEnd - relocCursor)) {
				break;
			}
			if (block->sizeOfBlock == sizeof(PEBaseRelocationBlock)) {
				break;
			}
			size_t entryCount = (block->sizeOfBlock - sizeof(PEBaseRelocationBlock)) / sizeof(uint16_t);
			auto *entries = reinterpret_cast<uint16_t *>(relocCursor + sizeof(PEBaseRelocationBlock));
			for (size_t i = 0; i < entryCount; ++i) {
				uint16_t entry = entries[i];
				uint16_t type = entry >> 12;
				uint16_t offset = entry & 0x0FFF;
				if (type == IMAGE_REL_BASED_ABSOLUTE)
					continue;
				uintptr_t target = reinterpret_cast<uintptr_t>(executable.imageBase) + block->virtualAddress + offset;
				switch (type) {
				case IMAGE_REL_BASED_DIR64: {
#ifdef WIBO_GUEST_64
					auto *addr = reinterpret_cast<uint64_t *>(target);
					*addr += static_cast<uint64_t>(executable.relocationDelta);
#else
					DEBUG_LOG("Unexpected 64-bit relocation at %08x\n", block->virtualAddress + offset);
#endif
					break;
				}
				case IMAGE_REL_BASED_HIGHLOW: {
					auto *addr = reinterpret_cast<uint32_t *>(target);
					*addr += static_cast<uint32_t>(executable.relocationDelta);
					break;
				}
				default:
					DEBUG_LOG("Unhandled relocation type %u at %08x\n", type, block->virtualAddress + offset);
					break;
				}
			}
			relocCursor += block->sizeOfBlock;
		}
	}

	executable.entryPoint =
		header32.addressOfEntryPoint ? executable.fromRVA<void>(header32.addressOfEntryPoint) : nullptr;
	if (dataImage) {
		for (const auto &section : executable.sections) {
			auto status = wibo::heap::virtualProtect(reinterpret_cast<void *>(section.base), section.size,
													 section.protect, nullptr);
			if (status != wibo::heap::VmStatus::Success) {
				kernel32::setLastError(wibo::heap::win32ErrorFromVmStatus(status));
				return false;
			}
		}
		executable.sectionsProtected = true;
	}

	(void)imageGuard.release();
	kernel32::setLastError(ERROR_SUCCESS);
	return true;
}

} // namespace

/**
 * Load a PE file into memory.
 *
 * @param file The file to load.
 * @param exec Whether to make the loaded image executable.
 */
bool wibo::Executable::loadPE(FILE *file, bool exec) {
	if (!file) {
		kernel32::setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	return loadPEFromSource(*this, PeInputView(file), exec);
}

bool wibo::Executable::imageMappingSize(FILE *file, size_t &size) {
	const DWORD lastError = kernel32::getLastError();
	ParsedPeImage image;
	if (!file) {
		kernel32::setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	if (!parsePeImage(PeInputView(file), true, image)) {
		return false;
	}
	size = image.optionalHeader.sizeOfImage;
	kernel32::setLastError(lastError);
	return true;
}

bool wibo::Executable::mapImage(FILE *file, void *requestedBase) {
	if (!file) {
		kernel32::setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	const DWORD lastError = kernel32::getLastError();
	if (!loadPEFromSource(*this, PeInputView(file), false, true, requestedBase)) {
		return false;
	}
	kernel32::setLastError(lastError);
	return true;
}

bool wibo::Executable::loadPE(std::span<const uint8_t> image, bool exec) {
	if (image.empty()) {
		kernel32::setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	return loadPEFromSource(*this, PeInputView(image), exec);
}

bool wibo::Executable::resolveImports(const ModuleSearch &search, ModuleInfo *importer) {
	if (!importer && mainModule && mainModule->executable.get() == this)
		importer = mainModule;
	auto finalizeSections = [this]() -> bool {
		if (!execMapped || sectionsProtected) {
			return true;
		}
		for (const auto &section : sections) {
			if (section.size == 0) {
				continue;
			}
			void *sectionAddress = reinterpret_cast<void *>(section.base);
			wibo::heap::VmStatus status =
				wibo::heap::virtualProtect(sectionAddress, section.size, section.protect, nullptr);
			if (status != wibo::heap::VmStatus::Success) {
				DEBUG_LOG("resolveImports: failed to set section protection at %p (size=%zu, protect=0x%x) status=%u\n",
						  sectionAddress, section.size, section.protect, static_cast<unsigned>(status));
				kernel32::setLastError(wibo::heap::win32ErrorFromVmStatus(status));
				return false;
			}
		}
		sectionsProtected = true;
		return true;
	};

	if (importsResolved || !execMapped) {
		importsResolved = true;
		importsResolving = false;
		if (!finalizeSections()) {
			return false;
		}
		return true;
	}
	if (importsResolving) {
		return true;
	}
	importsResolving = true;

	if (!importDirectoryRVA) {
		importsResolved = true;
		importsResolving = false;
		if (!finalizeSections()) {
			return false;
		}
		return true;
	}

	PEImportDirectoryEntry *dir = fromRVA<PEImportDirectoryEntry>(importDirectoryRVA);
	if (!dir) {
		importsResolved = true;
		importsResolving = false;
		if (!finalizeSections()) {
			return false;
		}
		return true;
	}

	while (dir->name) {
		char *dllName = fromRVA<char>(dir->name);
		DEBUG_LOG("DLL Name: %s\n", dllName);

		// Older PEs (NT 3.5-era) omit the ILT and only ship the IAT, which
		// on disk holds the hint/name RVAs that later get overwritten.
		uint32_t iltRVA = dir->importLookupTable ? dir->importLookupTable : dir->importAddressTable;
		GUEST_PTR *lookupTable = fromRVA<GUEST_PTR>(iltRVA);
		GUEST_PTR *addressTable = fromRVA<GUEST_PTR>(dir->importAddressTable);

		ModuleInfo *module = importer ? loadDependency(*importer, dllName, search) : loadModule(dllName, search);
		if (!module && (search.flags != 0 || kernel32::getLastError() != ERROR_MOD_NOT_FOUND)) {
			DEBUG_LOG("Failed to load import module %s\n", dllName);
			// lastError is set by loadModule
			importsResolved = false;
			importsResolving = false;
			return false;
		}

		while (*lookupTable) {
			GUEST_PTR lookup = *lookupTable;
			constexpr GUEST_PTR kOrdinalFlag = static_cast<GUEST_PTR>(1) << (sizeof(GUEST_PTR) * 8 - 1);
			if (lookup & kOrdinalFlag) {
				// Import by ordinal
				uint16_t ordinal = lookup & 0xFFFF;
				DEBUG_LOG("  Ordinal: %d\n", ordinal);
				void *func = module ? resolveFuncByOrdinal(module, ordinal, search, importer)
									: resolveMissingImportByOrdinal(dllName, ordinal);
				DEBUG_LOG("    -> %p\n", func);
				*addressTable = reinterpret_cast<uintptr_t>(func);
			} else {
				// Import by name
				PEHintNameTableEntry *hintName = fromRVA<PEHintNameTableEntry>(static_cast<uint32_t>(lookup));
				DEBUG_LOG("  Name: %s (IAT=%p)\n", hintName->name, addressTable);
				void *func = module ? resolveFuncByName(module, hintName->name, search, importer)
									: resolveMissingImportByName(dllName, hintName->name);
				DEBUG_LOG("    -> %p\n", func);
				*addressTable = reinterpret_cast<uintptr_t>(func);
			}
			++lookupTable;
			++addressTable;
		}
		++dir;
	}

	// TODO: actual delay loading from __delayLoadHelper2
	// if (delayImportDirectoryRVA) {
	// 	DEBUG_LOG("Processing delay import table at RVA %x\n", delayImportDirectoryRVA);
	// 	PEDelayImportDescriptor *delay = fromRVA<PEDelayImportDescriptor>(delayImportDirectoryRVA);
	// 	while (delay && delay->name) {
	// 		char *dllName = fromRVA<char>(delay->name);
	// 		DEBUG_LOG("Delay DLL Name: %s\n", dllName);
	// 		uint32_t *lookupTable = fromRVA<uint32_t>(delay->importNameTable);
	// 		uint32_t *addressTable = fromRVA<uint32_t>(delay->importAddressTable);
	// 		ModuleInfo *module = loadModule(dllName);
	// 		while (*lookupTable) {
	// 			uint32_t lookup = *lookupTable;
	// 			if (lookup & 0x80000000) {
	// 				uint16_t ordinal = lookup & 0xFFFF;
	// 				DEBUG_LOG("  Ordinal: %d (IAT=%p)\n", ordinal, addressTable);
	// 				void *func = module ? resolveFuncByOrdinal(module, ordinal)
	// 									: resolveMissingImportByOrdinal(dllName, ordinal);
	// 				*addressTable = reinterpret_cast<uintptr_t>(func);
	// 			} else {
	// 				PEHintNameTableEntry *hintName = fromRVA<PEHintNameTableEntry>(lookup);
	// 				DEBUG_LOG("  Name: %s\n", hintName->name);
	// 				void *func = module ? resolveFuncByName(module, hintName->name)
	// 									: resolveMissingImportByName(dllName, hintName->name);
	// 				*addressTable = reinterpret_cast<uintptr_t>(func);
	// 			}
	// 			++lookupTable;
	// 			++addressTable;
	// 		}
	// 		if (delay->moduleHandle) {
	// 			HMODULE *moduleSlot = fromRVA<HMODULE>(delay->moduleHandle);
	// 			if (moduleSlot) {
	// 				*moduleSlot = module;
	// 			}
	// 		}
	// 		++delay;
	// 	}
	// }

	importsResolved = true;
	importsResolving = false;
	if (!finalizeSections()) {
		importsResolved = false;
		return false;
	}
	return true;
}
