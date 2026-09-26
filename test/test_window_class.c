#include "test_assert.h"
#include <windows.h>

static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM first, LPARAM second) {
	(void)window;
	(void)message;
	(void)first;
	(void)second;
	return 0;
}

int main(void) {
	HINSTANCE instance = GetModuleHandleW(NULL);
	WCHAR name[] = L"wibo.fixture.class.\x4e2d";
	WCHAR menu[] = L"fixture.menu";
	WNDCLASSEXW definition;
	memset(&definition, 0, sizeof(definition));
	definition.cbSize = sizeof(definition);
	definition.style = CS_HREDRAW | CS_VREDRAW;
	definition.lpfnWndProc = windowProcedure;
	definition.cbClsExtra = 16;
	definition.cbWndExtra = 24;
	definition.hInstance = instance;
	definition.lpszClassName = name;
	definition.lpszMenuName = menu;
	ATOM atom = RegisterClassExW(&definition);
	TEST_CHECK(atom != 0);
	TEST_CHECK_EQ(0, RegisterClassExW(&definition));
	TEST_CHECK_EQ(ERROR_CLASS_ALREADY_EXISTS, GetLastError());
	name[0] = 'X';
	menu[0] = 'X';
	WNDCLASSEXW info;
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	TEST_CHECK(GetClassInfoExW(instance, L"WIBO.FIXTURE.CLASS.\x4e2d", &info));
	TEST_CHECK_EQ(sizeof(info), info.cbSize);
	TEST_CHECK_EQ(definition.style, info.style);
	TEST_CHECK(info.lpfnWndProc == windowProcedure);
	TEST_CHECK_EQ(16, info.cbClsExtra);
	TEST_CHECK_EQ(24, info.cbWndExtra);
	TEST_CHECK(info.hInstance == instance);
	TEST_CHECK(wcscmp(info.lpszMenuName, L"fixture.menu") == 0);
	TEST_CHECK(GetClassInfoExW(instance, MAKEINTRESOURCEW(atom), &info));
	TEST_CHECK(UnregisterClassW(MAKEINTRESOURCEW(atom), instance));
	TEST_CHECK(!GetClassInfoExW(instance, L"wibo.fixture.class.\x4e2d", &info));
	TEST_CHECK_EQ(ERROR_CLASS_DOES_NOT_EXIST, GetLastError());
	definition.lpszClassName = L"wibo.fixture.class.second";
	definition.lpszMenuName = MAKEINTRESOURCEW(42);
	definition.cbSize--;
	TEST_CHECK_EQ(0, RegisterClassExW(&definition));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	definition.cbSize++;
	definition.cbWndExtra = -1;
	TEST_CHECK_EQ(0, RegisterClassExW(&definition));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	definition.cbWndExtra = 0;
	TEST_CHECK(RegisterClassExW(&definition) != 0);
	TEST_CHECK(GetClassInfoExW(instance, definition.lpszClassName, &info));
	TEST_CHECK(info.lpszMenuName == MAKEINTRESOURCEW(42));
	TEST_CHECK(UnregisterClassW(definition.lpszClassName, instance));
	return 0;
}
