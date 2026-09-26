#pragma once

#include "types.h"

namespace oleaut32 {

LPWSTR WINAPI SysAllocString(LPCWSTR psz);
LPWSTR WINAPI SysAllocStringLen(LPCWSTR strIn, UINT ui);
void WINAPI SysFreeString(LPWSTR bstrString);
UINT WINAPI SysStringLen(LPWSTR pbstr);

} // namespace oleaut32
