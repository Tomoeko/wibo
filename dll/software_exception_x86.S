#include "macros.S"
#include "software_exception_x86_offsets.h"

#ifndef WIBO_GUEST_64
.text
.code32

ASM_GLOBAL(wiboRaiseException32, @function)
	pushfd
	pushad
	mov ebx, esp
	xor esi, esi
	jmp 1f
ASM_END(wiboRaiseException32)

ASM_GLOBAL(wiboRtlRaiseException32, @function)
	pushfd
	pushad
	mov ebx, esp
	mov esi, 1
	jmp 1f
ASM_END(wiboRtlRaiseException32)

ASM_GLOBAL(wiboRtlUnwind32, @function)
	pushfd
	pushad
	mov ebx, esp
	mov esi, 2
1:
	sub esp, WIBO_SEH32_SIZE+15
	and esp, -16
	cmp esp, dword ptr fs:[8]
	jb 9f
	mov edi, esp
	xor eax, eax
	mov ecx, WIBO_SEH32_SIZE/4
	cld
	rep stosd
	mov edi, esp
	mov dword ptr [edi+WIBO_SEH32_SAVED_STACK], ebx
	lea edx, [edi+WIBO_SEH32_CONTEXT]
	mov dword ptr [edx+WIBO_CTX32_FLAGS], 0x1002f
	mov eax, dword ptr [ebx]
	mov dword ptr [edx+WIBO_CTX32_EDI], eax
	mov eax, dword ptr [ebx+4]
	mov dword ptr [edx+WIBO_CTX32_ESI], eax
	mov eax, dword ptr [ebx+8]
	mov dword ptr [edx+WIBO_CTX32_EBP], eax
	mov eax, dword ptr [ebx+16]
	mov dword ptr [edx+WIBO_CTX32_EBX], eax
	mov eax, dword ptr [ebx+20]
	mov dword ptr [edx+WIBO_CTX32_EDX], eax
	mov eax, dword ptr [ebx+24]
	mov dword ptr [edx+WIBO_CTX32_ECX], eax
	mov eax, dword ptr [ebx+28]
	mov dword ptr [edx+WIBO_CTX32_EAX], eax
	mov eax, dword ptr [ebx+32]
	mov dword ptr [edx+WIBO_CTX32_EFLAGS], eax
	mov eax, dword ptr [ebx+36]
	mov dword ptr [edx+WIBO_CTX32_EIP], eax
	lea eax, [ebx+56]
	cmp esi, 1
	jne 2f
	lea eax, [ebx+44]
2:
	mov dword ptr [edx+WIBO_CTX32_ESP], eax
	xor eax, eax
	mov ax, cs
	mov dword ptr [edx+WIBO_CTX32_CS], eax
	mov ax, ss
	mov dword ptr [edx+WIBO_CTX32_SS], eax
	mov ax, ds
	mov dword ptr [edx+WIBO_CTX32_DS], eax
	mov ax, es
	mov dword ptr [edx+WIBO_CTX32_ES], eax
	mov ax, fs
	mov dword ptr [edx+WIBO_CTX32_FS], eax
	mov ax, gs
	mov dword ptr [edx+WIBO_CTX32_GS], eax
	fnsave [edx+WIBO_CTX32_FLOAT]
	frstor [edx+WIBO_CTX32_FLOAT]
	fxsave [edx+WIBO_CTX32_EXTENDED]
	push esi
	push edi
	call SYMBOL_NAME(thunk_entry_prepareSoftwareException32)
	add esp, 8
3:
	test eax, eax
	jnz 7f
	cmp dword ptr [edi+WIBO_SEH32_HANDLER_KIND], 0
	jne 5f
	lea eax, [edi+WIBO_SEH32_DISPATCHER]
	push eax
	lea eax, [edi+WIBO_SEH32_CONTEXT]
	push eax
	push dword ptr [edi+WIBO_SEH32_CURRENT]
	push dword ptr [edi+WIBO_SEH32_RECORD_POINTER]
	# The personality executes directly in guest mode. A nonlocal transfer
	# abandons only guest storage; every native helper already returned.
	call dword ptr [edi+WIBO_SEH32_HANDLER]
	add esp, 16
	jmp 6f
5:
	lea eax, [edi+WIBO_SEH32_POINTERS]
	push eax
	call dword ptr [edi+WIBO_SEH32_HANDLER]
6:
	mov edi, esp
	cld
	push eax
	push edi
	call SYMBOL_NAME(thunk_entry_finishSoftwareExceptionHandler32)
	add esp, 8
	jmp 3b
7:
	lea edi, [edi+WIBO_SEH32_CONTEXT]
	test dword ptr [edi+WIBO_CTX32_FLAGS], 0x20
	jz 8f
	fxrstor [edi+WIBO_CTX32_EXTENDED]
	jmp 10f
8:
	frstor [edi+WIBO_CTX32_FLOAT]
10:
	mov ax, word ptr [edi+WIBO_CTX32_DS]
	mov ds, ax
	mov ax, word ptr [edi+WIBO_CTX32_ES]
	mov es, ax
#ifndef __APPLE__
	# Darwin retains native hidden GS state for pthread/TSD access. The
	# validated guest selectors are unchanged; reloading zero could erase it.
	mov ax, word ptr [edi+WIBO_CTX32_FS]
	mov fs, ax
	mov ax, word ptr [edi+WIBO_CTX32_GS]
	mov gs, ax
#endif
	mov ebx, dword ptr [edi+WIBO_CTX32_EBX]
	mov esi, dword ptr [edi+WIBO_CTX32_ESI]
	mov ebp, dword ptr [edi+WIBO_CTX32_EBP]
	mov ecx, dword ptr [edi+WIBO_CTX32_ECX]
	mov edx, dword ptr [edi+WIBO_CTX32_EDX]
	mov eax, dword ptr [edi+WIBO_CTX32_ESP]
	sub eax, 4
	push dword ptr [edi+WIBO_CTX32_EIP]
	pop dword ptr [eax]
	push dword ptr [edi+WIBO_CTX32_EFLAGS]
	popfd
	mov esp, eax
	mov eax, dword ptr [edi+WIBO_CTX32_EAX]
	mov edi, dword ptr [edi+WIBO_CTX32_EDI]
	ret
9:
	# No dispatch object was written below the guest stack limit.
	mov esp, ebx
	push 0xc0000028
	call SYMBOL_NAME(thunk_entry_abortSoftwareException32)
	ud2
ASM_END(wiboRtlUnwind32)

ASM_GLOBAL(wiboExceptionGuard32, @function)
	mov eax, dword ptr [esp+4]
	mov eax, dword ptr [eax+4]
	test eax, 6
	jz 11f
	mov edx, dword ptr [esp+8]
	cmp dword ptr [edx+12], 0
	jne 12f
	# A search protector crossed by an unwind is simply removed.
	mov eax, 1
	ret
11:
	mov edx, dword ptr [esp+8]
	mov eax, 2
	jmp 13f
12:
	mov eax, 3
13:
	mov edx, dword ptr [edx+8]
	mov ecx, dword ptr [esp+16]
	mov dword ptr [ecx], edx
	ret
ASM_END(wiboExceptionGuard32)
#endif
