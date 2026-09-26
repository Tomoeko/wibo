#pragma once

#include "automation.h"
#include "types.h"

namespace oleaut32 {

HRESULT WINAPI VariantChangeType(AutomationVariant *destination, const AutomationVariant *source, WORD flags,
								 WORD type);
void WINAPI VariantInit(AutomationVariant *value);
HRESULT WINAPI VariantClear(AutomationVariant *value);

LPWSTR WINAPI SysAllocString(LPCWSTR psz);
LPWSTR WINAPI SysAllocStringLen(LPCWSTR strIn, UINT ui);
void WINAPI SysFreeString(LPWSTR bstrString);
UINT WINAPI SysStringLen(LPWSTR pbstr);

} // namespace oleaut32
