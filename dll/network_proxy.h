#pragma once

#include "types.h"

namespace wibo::network {
bool isManagerClass(const GUID *classId);
HRESULT createManager(LPVOID outer, DWORD context, const GUID *iid, GUEST_PTR *result);
} // namespace wibo::network

namespace network_proxy {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result);
ULONG WINAPI AddRef(GUEST_PTR self);
ULONG WINAPI Release(GUEST_PTR self);
HRESULT WINAPI GetConnectivity(GUEST_PTR self, DWORD *result);
HRESULT WINAPI IsConnected(GUEST_PTR self, SHORT *result);
HRESULT WINAPI IsConnectedToInternet(GUEST_PTR self, SHORT *result);
HRESULT WINAPI Unavailable1(GUEST_PTR a);
HRESULT WINAPI Unavailable2(GUEST_PTR a, GUEST_PTR b);
HRESULT WINAPI Unavailable3(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c);
HRESULT WINAPI Unavailable4(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d);
HRESULT WINAPI Unavailable6(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d, GUEST_PTR e, GUEST_PTR f);
HRESULT WINAPI Unavailable9(GUEST_PTR a, GUEST_PTR b, GUEST_PTR c, GUEST_PTR d, GUEST_PTR e, GUEST_PTR f, GUEST_PTR g,
							GUEST_PTR h, GUEST_PTR i);
#ifdef WIBO_GUEST_64
HRESULT WINAPI UnavailableGuid(GUEST_PTR self, GUEST_PTR guid, GUEST_PTR result);
#else
HRESULT WINAPI UnavailableGuid(GUEST_PTR self, DWORD a, DWORD b, DWORD c, DWORD d, GUEST_PTR result);
#endif
} // namespace network_proxy
