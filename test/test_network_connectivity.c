#define COBJMACROS
#include "test_assert.h"
#include <netlistmgr.h>
#include <windows.h>

int main(void) {
	TEST_CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
	INetworkListManager *manager = NULL;
	TEST_CHECK_EQ(S_OK, CoCreateInstance(&CLSID_NetworkListManager, NULL, CLSCTX_ALL, &IID_INetworkListManager,
										 (void **)&manager));
	TEST_CHECK(manager != NULL);
	IUnknown *unknown = NULL;
	TEST_CHECK_EQ(S_OK, INetworkListManager_QueryInterface(manager, &IID_IUnknown, (void **)&unknown));
	TEST_CHECK(unknown == (IUnknown *)manager);
	TEST_CHECK_EQ(1, IUnknown_Release(unknown));
	NLM_CONNECTIVITY state = 0;
	TEST_CHECK_EQ(S_OK, INetworkListManager_GetConnectivity(manager, &state));
	TEST_CHECK_EQ(0, state & ~0x773);
	VARIANT_BOOL connected = 1, internet = 1;
	TEST_CHECK_EQ(S_OK, INetworkListManager_IsConnected(manager, &connected));
	TEST_CHECK_EQ(S_OK, INetworkListManager_IsConnectedToInternet(manager, &internet));
	TEST_CHECK(connected == VARIANT_TRUE || connected == VARIANT_FALSE);
	TEST_CHECK(internet == VARIANT_TRUE || internet == VARIANT_FALSE);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_INTERNET, state);
		TEST_CHECK_EQ(VARIANT_TRUE, connected);
		TEST_CHECK_EQ(VARIANT_TRUE, internet);
		const GUID missing = {0x1234, 0x5678, 0, {0}};
		unknown = (IUnknown *)(uintptr_t)1;
		TEST_CHECK_EQ(E_NOINTERFACE, INetworkListManager_QueryInterface(manager, &missing, (void **)&unknown));
		TEST_CHECK(unknown == NULL);
		INetwork *network = NULL;
		TEST_CHECK_EQ(E_NOTIMPL, INetworkListManager_GetNetwork(manager, missing, &network));
		TEST_CHECK_EQ(E_POINTER, INetworkListManager_GetConnectivity(manager, NULL));
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NETWORK_RESPONSE", "truncated"));
		state = 0x1234;
		TEST_CHECK_EQ(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), INetworkListManager_GetConnectivity(manager, &state));
		TEST_CHECK_EQ(0x1234, state);
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NETWORK_RESPONSE", "bad-flags"));
		TEST_CHECK_EQ(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), INetworkListManager_GetConnectivity(manager, &state));
		TEST_CHECK_EQ(0x1234, state);
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NETWORK_RESPONSE", "failed"));
		TEST_CHECK_EQ(E_ACCESSDENIED, INetworkListManager_GetConnectivity(manager, &state));
		INetworkListManager *failed = (INetworkListManager *)(uintptr_t)1;
		TEST_CHECK_EQ(E_ACCESSDENIED, CoCreateInstance(&CLSID_NetworkListManager, NULL, CLSCTX_ALL,
													   &IID_INetworkListManager, (void **)&failed));
		TEST_CHECK(failed == NULL);
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NETWORK_RESPONSE", NULL));
	}
	TEST_CHECK_EQ(0, INetworkListManager_Release(manager));
	CoUninitialize();
	return 0;
}
