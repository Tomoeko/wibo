#include "ntdll.h"

#ifdef WIBO_GUEST_64
#include "common.h"
#include "context.h"
#include "kernel32/internal.h"
#include "kernel32/thread_context.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace {
constexpr std::array<ULONGLONG CONTEXT64::*, 16> kIntegerRegisters = {
	&CONTEXT64::Rax, &CONTEXT64::Rcx, &CONTEXT64::Rdx, &CONTEXT64::Rbx, &CONTEXT64::Rsp, &CONTEXT64::Rbp,
	&CONTEXT64::Rsi, &CONTEXT64::Rdi, &CONTEXT64::R8,  &CONTEXT64::R9,	&CONTEXT64::R10, &CONTEXT64::R11,
	&CONTEXT64::R12, &CONTEXT64::R13, &CONTEXT64::R14, &CONTEXT64::R15};
constexpr BYTE kExceptionHandler = 1, kUnwindHandler = 2, kChained = 4;

[[noreturn]] void invalidUnwind(const char *reason) {
	std::fprintf(stderr, "wibo: cannot unwind frame: %s\n", reason);
	kernel32::exitInternal(0xc00000ff); // STATUS_BAD_FUNCTION_TABLE
}

template <typename T> T readValue(ULONGLONG address) {
	T value{};
	std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
	return value;
}

ULONGLONG &integerRegister(CONTEXT64 &context, unsigned index) { return context.*kIntegerRegisters[index]; }

void restoreInteger(CONTEXT64 &context, KNONVOLATILE_CONTEXT_POINTERS *pointers, unsigned reg, ULONGLONG address) {
	integerRegister(context, reg) = readValue<ULONGLONG>(address);
	if (pointers)
		pointers->IntegerContext[reg] = address;
}

struct UnwindInfo {
	const BYTE *bytes;
	BYTE version, flags, prologue, count, frameRegister, frameOffset;
	UnwindInfo(ULONGLONG base, const RUNTIME_FUNCTION &entry)
		: bytes(reinterpret_cast<const BYTE *>(base + entry.UnwindData)), version(bytes[0] & 7), flags(bytes[0] >> 3),
		  prologue(bytes[1]), count(bytes[2]), frameRegister(bytes[3] & 15), frameOffset(bytes[3] >> 4) {
		if ((entry.UnwindData & 3) || (version != 1 && version != 2) || (flags & ~7) ||
			((flags & kChained) && (flags & (kExceptionHandler | kUnwindHandler))))
			invalidUnwind("unsupported or malformed unwind header");
	}
	[[nodiscard]] const BYTE *code(unsigned index) const { return bytes + 4 + index * 2; }
	[[nodiscard]] ULONGLONG trailer() const { return reinterpret_cast<ULONGLONG>(code((count + 1U) & ~1U)); }
	[[nodiscard]] unsigned slots(unsigned index) const {
		const auto *instruction = code(index);
		const unsigned op = instruction[1] & 15, detail = instruction[1] >> 4;
		unsigned length;
		switch (op) {
		case 0:
		case 2:
		case 3:
		case 10:
			length = 1;
			break;
		case 1:
			if (detail > 1)
				invalidUnwind("invalid allocation encoding");
			length = detail ? 3 : 2;
			break;
		case 4:
		case 8:
			length = 2;
			break;
		case 5:
		case 9:
			length = 3;
			break;
		case 6:
			if (version != 2)
				invalidUnwind("epilogue descriptor requires version 2");
			length = 2;
			break;
		default:
			invalidUnwind("unsupported unwind operation");
		}
		if (length > count - index)
			invalidUnwind("truncated unwind operation");
		return length;
	}
};

ULONGLONG establisherFrame(const UnwindInfo &info, ULONGLONG offset, CONTEXT64 &context) {
	if (!info.frameRegister)
		return context.Rsp;
	bool established = offset >= info.prologue || (info.flags & kChained);
	for (unsigned index = 0; !established && index < info.count; index += info.slots(index)) {
		const auto *code = info.code(index);
		if ((code[1] & 15) == 3)
			established = offset >= code[0];
	}
	return established ? integerRegister(context, info.frameRegister) - info.frameOffset * 16U : context.Rsp;
}

std::optional<ULONGLONG> describedEpilogue(const UnwindInfo &info, const RUNTIME_FUNCTION &entry,
										   ULONGLONG relativePc) {
	if (!info.count || (info.code(0)[1] & 15) != 6)
		return std::nullopt;
	const unsigned size = info.code(0)[0];
	if (!size || size > entry.EndAddress - entry.BeginAddress)
		invalidUnwind("invalid epilogue size");
	if (((info.code(0)[1] >> 4) & 1) && relativePc >= entry.EndAddress - size && relativePc < entry.EndAddress)
		return relativePc - (entry.EndAddress - size);
	for (unsigned index = 1; index < info.count && (info.code(index)[1] & 15) == 6; ++index) {
		const auto *code = info.code(index);
		const unsigned distance = code[0] + (code[1] >> 4) * 256U;
		if (!distance)
			break;
		if (distance > entry.EndAddress - entry.BeginAddress)
			invalidUnwind("epilogue is outside the function");
		const auto start = entry.EndAddress - distance;
		if (relativePc >= start && relativePc - start < size)
			return relativePc - start;
	}
	return std::nullopt;
}

RUNTIME_FUNCTION primaryEntry(ULONGLONG base, RUNTIME_FUNCTION entry) {
	for (unsigned depth = 0; depth < 64; ++depth) {
		UnwindInfo info(base, entry);
		if (!(info.flags & kChained))
			return entry;
		entry = readValue<RUNTIME_FUNCTION>(info.trailer());
	}
	invalidUnwind("too many chained records");
}

void unwindDescribedEpilogue(ULONGLONG base, RUNTIME_FUNCTION entry, ULONGLONG offset, CONTEXT64 &context,
							 KNONVOLATILE_CONTEXT_POINTERS *pointers) {
	for (unsigned depth = 0; depth < 64; ++depth) {
		UnwindInfo info(base, entry);
		unsigned index = 0;
		for (; index < info.count; index += info.slots(index)) {
			const auto op = info.code(index)[1] & 15;
			if (op == 0 || op == 10)
				break;
		}
		if (index == info.count && (info.flags & kChained)) {
			entry = readValue<RUNTIME_FUNCTION>(info.trailer());
			continue;
		}
		// Version 2 scopes begin after allocation cleanup. Remaining pushes mirror epilogue pops.
		unsigned position = 0;
		while (index < info.count && (info.code(index)[1] & 15) == 0) {
			const unsigned reg = info.code(index)[1] >> 4;
			if (position >= offset) {
				restoreInteger(context, pointers, reg, context.Rsp);
				context.Rsp += 8;
			}
			position += reg >= 8 ? 2 : 1;
			++index;
		}
		if (index < info.count && info.code(index)[1] == 2) {
			// A one-slot allocation can represent the flags push removed by a volatile pop.
			if (position >= offset)
				context.Rsp += 8;
			++index;
		}
		if (index < info.count) {
			if ((info.code(index)[1] & 15) != 10 || (info.code(index)[1] >> 4) > 1 || index + 1 != info.count)
				invalidUnwind("unsupported epilogue unwind operations");
			context.Rip = readValue<ULONGLONG>(context.Rsp);
			context.Rsp = readValue<ULONGLONG>(context.Rsp + 24);
		} else {
			context.Rip = readValue<ULONGLONG>(context.Rsp);
			context.Rsp += 8;
		}
		return;
	}
	invalidUnwind("too many chained records");
}

bool unwindEpilogue(ULONGLONG base, ULONGLONG pc, const RUNTIME_FUNCTION &entry, const UnwindInfo &info,
					CONTEXT64 &context, KNONVOLATILE_CONTEXT_POINTERS *pointers) {
	const auto end = base + entry.EndAddress;
	if (pc < base + entry.BeginAddress || pc >= end)
		return false;
	// Decode into temporary state: an ordinary body instruction must not change the context.
	auto next = context;
	KNONVOLATILE_CONTEXT_POINTERS locations{};
	if (pointers)
		locations = *pointers;
	const auto available = [&](size_t count) { return pc < end && count <= end - pc; };
	const auto byte = [&](size_t offset) { return readValue<BYTE>(pc + offset); };
	if (available(4) && byte(0) == 0x48 && byte(1) == 0x83 && byte(2) == 0xc4) {
		next.Rsp += static_cast<int8_t>(byte(3));
		pc += 4;
	} else if (available(7) && byte(0) == 0x48 && byte(1) == 0x81 && byte(2) == 0xc4) {
		next.Rsp += readValue<int32_t>(pc + 3);
		pc += 7;
	} else if (available(4) && (byte(0) & 0xfe) == 0x48 && byte(1) == 0x8d) {
		const unsigned modrm = byte(2), reg = (modrm & 7) + (byte(0) & 1) * 8;
		if (!info.frameRegister || reg != info.frameRegister || (modrm & 0x38) != 0x20 || (modrm & 7) == 4)
			return false;
		if ((modrm & 0xc0) == 0x40) {
			next.Rsp = integerRegister(next, reg) + static_cast<int8_t>(byte(3));
			pc += 4;
		} else if (available(7) && (modrm & 0xc0) == 0x80) {
			next.Rsp = integerRegister(next, reg) + readValue<int32_t>(pc + 3);
			pc += 7;
		} else
			return false;
	}
	while (available(1)) {
		unsigned prefix = 0, extension = 0;
		if ((byte(0) & 0xf0) == 0x40) {
			if (!available(2))
				return false;
			prefix = 1;
			extension = (byte(0) & 1) * 8;
		}
		if ((byte(prefix) & 0xf8) != 0x58)
			break;
		const unsigned reg = (byte(prefix) & 7) + extension;
		if (reg == 4)
			return false;
		restoreInteger(next, pointers ? &locations : nullptr, reg, next.Rsp);
		next.Rsp += 8;
		pc += prefix + 1;
	}
	if (!available(1))
		return false;
	bool terminal =
		byte(0) == 0xc3 || (byte(0) == 0xc2 && available(3)) || (byte(0) == 0xf3 && available(2) && byte(1) == 0xc3);
	if ((byte(0) == 0xe9 && available(5)) || (byte(0) == 0xeb && available(2))) {
		const unsigned length = byte(0) == 0xe9 ? 5 : 2;
		const auto displacement = length == 5 ? readValue<int32_t>(pc + 1) : static_cast<int8_t>(byte(1));
		const ULONGLONG target = pc + length + displacement;
		terminal = target < base + entry.BeginAddress || target >= end;
		if (terminal) {
			ULONGLONG targetBase = 0;
			const auto *targetEntry = ntdll::RtlLookupFunctionEntry(target, &targetBase, nullptr);
			if (targetEntry && targetBase == base) {
				const auto primary = primaryEntry(base, entry);
				const auto targetPrimary = primaryEntry(base, *targetEntry);
				terminal = primary.BeginAddress != targetPrimary.BeginAddress || target == base + primary.BeginAddress;
			}
		} else
			terminal = target == base + entry.BeginAddress && !(info.flags & kChained);
	}
	unsigned prefix = available(2) && (byte(0) & 0xf0) == 0x40 ? 1 : 0;
	if (available(prefix + 2) && byte(prefix) == 0xff && (byte(prefix + 1) & 0x38) == 0x20) {
		const unsigned modrm = byte(prefix + 1);
		// Memory tail jumps have mod=00; register tail jumps require REX.W.
		terminal = (modrm & 0xc0) == 0 || (prefix && (byte(0) & 8) && (modrm & 0xc0) == 0xc0);
	}
	if (!terminal)
		return false;
	next.Rip = readValue<ULONGLONG>(next.Rsp);
	next.Rsp += 8;
	context = next;
	if (pointers)
		*pointers = locations;
	return true;
}
} // namespace

namespace ntdll {
PVOID WINAPI RtlVirtualUnwind(DWORD handlerType, ULONGLONG imageBase, ULONGLONG controlPc,
							  RUNTIME_FUNCTION *functionEntry, CONTEXT64 *context, PVOID *handlerData, ULONGLONG *frame,
							  KNONVOLATILE_CONTEXT_POINTERS *pointers) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlVirtualUnwind(%u, %llx, %llx, %p, %p, %p, %p, %p)\n", handlerType, imageBase, controlPc,
			  functionEntry, context, handlerData, frame, pointers);
	if (!functionEntry || !context || !frame || !handlerData)
		invalidUnwind("missing required argument");
	RUNTIME_FUNCTION entry = *functionEntry;
	UnwindInfo initial(imageBase, entry);
	ULONGLONG offset = controlPc - imageBase - entry.BeginAddress;
	*frame = establisherFrame(initial, offset, *context);
	if (offset >= initial.prologue) {
		if (initial.version == 1) {
			if (unwindEpilogue(imageBase, controlPc, entry, initial, *context, pointers))
				return nullptr;
		} else if (const auto epilogueOffset = describedEpilogue(initial, entry, controlPc - imageBase)) {
			unwindDescribedEpilogue(imageBase, entry, *epilogueOffset, *context, pointers);
			return nullptr;
		}
	}
	std::array<DWORD, 64> visited{};
	unsigned depth = 0;
	bool machineFrame = false;
	for (;;) {
		if (depth == visited.size())
			invalidUnwind("too many chained records");
		for (unsigned i = 0; i < depth; ++i)
			if (visited[i] == entry.UnwindData)
				invalidUnwind("cyclic unwind chain");
		visited[depth++] = entry.UnwindData;
		UnwindInfo info(imageBase, entry);
		for (unsigned index = 0; index < info.count;) {
			const auto *code = info.code(index);
			const unsigned op = code[1] & 15, reg = code[1] >> 4, slots = info.slots(index);
			if (op != 6 && offset >= code[0]) {
				ULONGLONG displacement = 0;
				if (slots >= 2)
					displacement = readValue<WORD>(reinterpret_cast<ULONGLONG>(code + 2));
				if (slots == 3)
					displacement |= ULONGLONG(readValue<WORD>(reinterpret_cast<ULONGLONG>(code + 4))) << 16;
				switch (op) {
				case 0:
					restoreInteger(*context, pointers, reg, context->Rsp);
					context->Rsp += 8;
					break;
				case 1:
					context->Rsp += displacement * (reg ? 1 : 8);
					break;
				case 2:
					context->Rsp += reg * 8 + 8;
					break;
				case 3:
					if (!info.frameRegister || reg)
						invalidUnwind("invalid frame register operation");
					context->Rsp = integerRegister(*context, info.frameRegister) - info.frameOffset * 16U;
					break;
				case 4:
				case 5:
					restoreInteger(*context, pointers, reg, *frame + displacement * (op == 4 ? 8 : 1));
					break;
				case 8:
				case 9: {
					const auto address = *frame + displacement * (op == 8 ? 16 : 1);
					context->FltSave.XmmRegisters[reg] = readValue<M128A>(address);
					if (pointers)
						pointers->FloatingContext[reg] = address;
					break;
				}
				case 10: {
					if (reg > 1)
						invalidUnwind("invalid machine frame operation");
					const auto address = context->Rsp + reg * 8;
					context->Rip = readValue<ULONGLONG>(address);
					context->Rsp = readValue<ULONGLONG>(address + 24);
					machineFrame = true;
					break;
				}
				default:
					invalidUnwind("unsupported unwind operation");
				}
			}
			index += slots;
		}
		if (info.flags & kChained) {
			entry = readValue<RUNTIME_FUNCTION>(info.trailer());
			offset = std::numeric_limits<ULONGLONG>::max();
			continue;
		}
		if (!machineFrame) {
			context->Rip = readValue<ULONGLONG>(context->Rsp);
			context->Rsp += 8;
		}
		if (controlPc - imageBase - entry.BeginAddress >= info.prologue &&
			(info.flags & handlerType & (kExceptionHandler | kUnwindHandler))) {
			*handlerData = reinterpret_cast<PVOID>(info.trailer() + sizeof(DWORD));
			return reinterpret_cast<PVOID>(imageBase + readValue<DWORD>(info.trailer()));
		}
		return nullptr;
	}
}
} // namespace ntdll
#endif
