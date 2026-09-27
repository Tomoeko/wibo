#include "../user32.h"

#include "common.h"
#include "context.h"
#include "kernel32/winnls.h"

namespace user32 {

LPSTR WINAPI CharNextExA(WORD codePage, LPCSTR current, DWORD flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CharNextExA(%u, %p, 0x%x)\n", codePage, current, flags);
	if (!*current)
		return const_cast<LPSTR>(current);
	const BOOL leadByte = kernel32::IsDBCSLeadByteEx(codePage, static_cast<BYTE>(*current));
	return const_cast<LPSTR>(current + (leadByte && current[1] ? 2 : 1));
}

} // namespace user32
