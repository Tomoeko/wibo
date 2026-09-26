#define CINTERFACE
#define COBJMACROS
#include <windows.h>

#include "test_assert.h"
#include <oleauto.h>
#include <wbemcli.h>

int main(void) {
	IWbemLocator *locator = NULL;
	TEST_CHECK_EQ(S_OK, CoInitialize(NULL));
	TEST_CHECK_EQ(
		S_OK, CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&locator));
	TEST_CHECK(locator != NULL);
	IUnknown *identity = NULL;
	TEST_CHECK_EQ(S_OK, IWbemLocator_QueryInterface(locator, &IID_IUnknown, (void **)&identity));
	TEST_CHECK((void *)identity == (void *)locator);
	IUnknown_Release(identity);
	void *unsupported = (void *)1;
	TEST_CHECK_EQ(E_NOINTERFACE, IWbemLocator_QueryInterface(locator, &IID_IDispatch, &unsupported));
	if (getenv("WIBO_FIXTURE_PROVIDER"))
		TEST_CHECK(unsupported == NULL);
	BSTR space = SysAllocString(L"root\\cimv2");
	IWbemServices *services = NULL;
	TEST_CHECK_EQ(S_OK, IWbemLocator_ConnectServer(locator, space, NULL, NULL, NULL, 0, NULL, NULL, &services));
	TEST_CHECK_EQ(S_OK, CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
										  RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE));
	SysFreeString(space);
	IWbemLocator_Release(locator);
	BSTR language = SysAllocString(L"WQL");
	BSTR query = SysAllocString(L"SELECT Caption,Version FROM Win32_OperatingSystem");
	IEnumWbemClassObject *enumeration = NULL;
	TEST_CHECK_EQ(S_OK, IWbemServices_ExecQuery(services, language, query, 0, NULL, &enumeration));
	SysFreeString(language);
	SysFreeString(query);
	IWbemServices_Release(services);
	IWbemClassObject *object = NULL;
	ULONG returned = 42;
	TEST_CHECK_EQ(S_OK, IEnumWbemClassObject_Next(enumeration, WBEM_INFINITE, 1, &object, &returned));
	TEST_CHECK_EQ(1, returned);
	TEST_CHECK(object != NULL);
	VARIANT value;
	VariantInit(&value);
	CIMTYPE type = 0;
	LONG flavor = 0;
	TEST_CHECK_EQ(S_OK, IWbemClassObject_Get(object, L"cApTiOn", 0, &value, &type, &flavor));
	TEST_CHECK_EQ(VT_BSTR, V_VT(&value));
	TEST_CHECK_EQ(CIM_STRING, type);
	TEST_CHECK(SysStringLen(V_BSTR(&value)) != 0);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		const WCHAR expected[] = {0x4E2D, 0, 0xD83D, 0xDE00};
		TEST_CHECK_EQ(4, SysStringLen(V_BSTR(&value)));
		TEST_CHECK(memcmp(expected, V_BSTR(&value), sizeof(expected)) == 0);
		TEST_CHECK_EQ(0x20, flavor);
	}
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(VT_EMPTY, V_VT(&value));
	TEST_CHECK_EQ((HRESULT)WBEM_E_NOT_FOUND, IWbemClassObject_Get(object, L"MissingProperty", 0, &value, NULL, NULL));
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(S_OK, IWbemClassObject_Get(object, L"Flags", 0, &value, &type, NULL));
		TEST_CHECK_EQ(VT_UI4, V_VT(&value));
		TEST_CHECK_U64_EQ(0xfedcba98, V_UI4(&value));
		VariantClear(&value);
		TEST_CHECK_EQ(S_OK, IWbemClassObject_Get(object, L"Signed", 0, &value, &type, NULL));
		TEST_CHECK_EQ(VT_I8, V_VT(&value));
		TEST_CHECK_EQ(-0x100000001LL, V_I8(&value));
		VariantClear(&value);
		TEST_CHECK_EQ(S_OK, IWbemClassObject_Get(object, L"Optional", 0, &value, &type, NULL));
		TEST_CHECK_EQ(VT_NULL, V_VT(&value));
		TEST_CHECK_EQ(CIM_STRING, type);
		VariantClear(&value);
		TEST_CHECK_EQ((HRESULT)WBEM_E_NOT_SUPPORTED,
					  IWbemClassObject_Get(object, L"Unsupported", 0, &value, NULL, NULL));
	}
	IWbemClassObject *copy = object;
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(S_OK, IWbemClassObject_Clone(object, &copy));
		IWbemClassObject_Release(object);
	}
	TEST_CHECK_EQ(S_OK, IWbemClassObject_Get(copy, L"Version", 0, &value, NULL, NULL));
	TEST_CHECK_EQ(VT_BSTR, V_VT(&value));
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(S_OK, IWbemClassObject_BeginEnumeration(copy, 0));
	BSTR property = NULL;
	TEST_CHECK_EQ(S_OK, IWbemClassObject_Next(copy, 0, &property, &value, &type, NULL));
	TEST_CHECK(property != NULL);
	SysFreeString(property);
	TEST_CHECK_EQ(S_OK, VariantClear(&value));
	TEST_CHECK_EQ(S_OK, IWbemClassObject_EndEnumeration(copy));
	IWbemClassObject_Release(copy);
	object = NULL;
	returned = 42;
	TEST_CHECK_EQ(WBEM_S_FALSE, IEnumWbemClassObject_Next(enumeration, WBEM_INFINITE, 1, &object, &returned));
	TEST_CHECK_EQ(0, returned);
	TEST_CHECK_EQ(S_OK, IEnumWbemClassObject_Reset(enumeration));
	TEST_CHECK_EQ(S_OK, IEnumWbemClassObject_Next(enumeration, WBEM_INFINITE, 1, &object, &returned));
	IWbemClassObject_Release(object);
	IEnumWbemClassObject_Release(enumeration);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(S_OK, CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator,
											 (void **)&locator));
		space = SysAllocString(L"root\\cimv2");
		TEST_CHECK_EQ(S_OK, IWbemLocator_ConnectServer(locator, space, NULL, NULL, NULL, 0, NULL, NULL, &services));
		SysFreeString(space);
		IWbemLocator_Release(locator);
		const WCHAR *queries[] = {L"truncated", L"trailing", L"bad-type", L"exit-failed", L"failed", L"timeout"};
		for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
			language = SysAllocString(L"WQL");
			query = SysAllocString(queries[i]);
			TEST_CHECK_EQ(S_OK, IWbemServices_ExecQuery(services, language, query, WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
														&enumeration));
			SysFreeString(language);
			SysFreeString(query);
			returned = 99;
			TEST_CHECK_EQ(WBEM_S_TIMEDOUT, IEnumWbemClassObject_Next(enumeration, 0, 1, &object, &returned));
			TEST_CHECK_EQ(0, returned);
			HRESULT expected = i == 4	? (HRESULT)WBEM_E_INVALID_CLASS
							   : i == 5 ? (HRESULT)WBEM_S_TIMEDOUT
										: (HRESULT)WBEM_E_FAILED;
			TEST_CHECK_EQ(expected, IEnumWbemClassObject_Next(enumeration, i == 5 ? (LONG)50 : (LONG)2000, 1, &object,
															  &returned));
			TEST_CHECK_EQ(0, returned);
			IEnumWbemClassObject_Release(enumeration);
		}
		IWbemServices_Release(services);
	}
	CoUninitialize();
	return 0;
}
