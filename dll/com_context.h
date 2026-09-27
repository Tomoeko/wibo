#pragma once

#include "types.h"

namespace wibo::com {
void beginApartment(bool multithreaded);
void endApartment();
void releaseThreadContext();
HRESULT currentContextToken(ULONG_PTR *token);
} // namespace wibo::com

namespace com_context {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result);
ULONG WINAPI AddRef(GUEST_PTR self);
ULONG WINAPI Release(GUEST_PTR self);
HRESULT WINAPI SetProperty(GUEST_PTR self, const GUID *id, DWORD flags, GUEST_PTR value);
HRESULT WINAPI RemoveProperty(GUEST_PTR self, const GUID *id);
HRESULT WINAPI GetProperty(GUEST_PTR self, const GUID *id, DWORD *flags, GUEST_PTR *value);
HRESULT WINAPI EnumContextProps(GUEST_PTR self, GUEST_PTR *result);
void WINAPI Reserved(GUEST_PTR self);
} // namespace com_context
