#pragma once

#include "automation.h"
#include "types.h"

namespace wibo::management {
HRESULT setProxyBlanket(GUEST_PTR proxy, DWORD authentication, DWORD authorization, LPCWSTR principal, DWORD level,
						DWORD impersonation, GUEST_PTR identity, DWORD capabilities);
HRESULT createLocator(const GUID *classId, LPVOID outer, DWORD context, const GUID *iid, GUEST_PTR *result);
} // namespace wibo::management

namespace wmi_proxy {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result);
ULONG WINAPI AddRef(GUEST_PTR self);
ULONG WINAPI Release(GUEST_PTR self);
HRESULT WINAPI ConnectServer(GUEST_PTR self, LPCWSTR resource, LPCWSTR user, LPCWSTR password, LPCWSTR locale,
							 LONG flags, LPCWSTR authority, GUEST_PTR context, GUEST_PTR *result);
HRESULT WINAPI CreateInstanceEnum(GUEST_PTR self, LPCWSTR className, LONG flags, GUEST_PTR context, GUEST_PTR *result);
HRESULT WINAPI ExecQuery(GUEST_PTR self, LPCWSTR language, LPCWSTR query, LONG flags, GUEST_PTR context,
						 GUEST_PTR *result);
HRESULT WINAPI Reset(GUEST_PTR self);
HRESULT WINAPI Next(GUEST_PTR self, LONG timeout, ULONG count, GUEST_PTR *objects, ULONG *returned);
HRESULT WINAPI Clone(GUEST_PTR self, GUEST_PTR *result);
HRESULT WINAPI Skip(GUEST_PTR self, LONG timeout, ULONG count);
HRESULT WINAPI Get(GUEST_PTR self, LPCWSTR name, LONG flags, AutomationVariant *value, LONG *type, LONG *flavor);
HRESULT WINAPI BeginEnumeration(GUEST_PTR self, LONG flags);
HRESULT WINAPI NextProperty(GUEST_PTR self, LONG flags, GUEST_PTR *name, AutomationVariant *value, LONG *type,
							LONG *flavor);
HRESULT WINAPI EndEnumeration(GUEST_PTR self);

// Unsupported interface slots retain their own guest stack arity.
HRESULT WINAPI Unavailable1(GUEST_PTR a);
HRESULT WINAPI Unavailable2(GUEST_PTR a, GUEST_PTR b);
HRESULT WINAPI Unavailable3(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c);
HRESULT WINAPI Unavailable5(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d, GUEST_PTR e);
HRESULT WINAPI Unavailable6(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d, GUEST_PTR e, GUEST_PTR f);
HRESULT WINAPI Unavailable8(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d, GUEST_PTR e, GUEST_PTR f, GUEST_PTR g,
							GUEST_PTR h);
} // namespace wmi_proxy
