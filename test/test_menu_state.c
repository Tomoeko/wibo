#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

int main(void) {
	const DWORD sentinel = 0x23456789;
	HMENU menu;
	HMENU popup;
	MENUITEMINFOW info;
	WCHAR text[8];

	SetLastError(sentinel);
	menu = CreateMenu();
	TEST_CHECK(menu != NULL);
	TEST_CHECK_EQ(sentinel, GetLastError());
	popup = CreatePopupMenu();
	TEST_CHECK(popup != NULL);
	TEST_CHECK(popup != menu);
	TEST_CHECK_EQ(0, GetMenuItemCount(menu));
	TEST_CHECK(AppendMenuA(menu, MF_STRING, 101, "Alpha"));
	TEST_CHECK(AppendMenuA(menu, MF_POPUP, (UINT_PTR)popup, "Child"));
	TEST_CHECK(AppendMenuA(popup, MF_STRING, 202, "Nested"));
	TEST_CHECK_EQ(2, GetMenuItemCount(menu));
	TEST_CHECK_EQ(1, GetMenuItemCount(popup));
	TEST_CHECK_EQ((UINT_PTR)popup, (UINT_PTR)GetSubMenu(menu, 1));
	TEST_CHECK_EQ(0, (UINT_PTR)GetSubMenu(menu, 0));

	TEST_CHECK_EQ(0, CheckMenuItem(menu, 101, MF_BYCOMMAND | MF_CHECKED));
	TEST_CHECK_EQ(MF_CHECKED, CheckMenuItem(menu, 101, MF_BYCOMMAND | MF_CHECKED));
	TEST_CHECK_EQ(0, EnableMenuItem(menu, 101, MF_BYCOMMAND | MF_GRAYED));
	TEST_CHECK_EQ(MF_GRAYED, EnableMenuItem(menu, 101, MF_BYCOMMAND | MF_GRAYED));
	TEST_CHECK_EQ(sentinel, GetLastError());

	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
	TEST_CHECK(GetMenuItemInfoW(menu, 0, TRUE, &info));
	TEST_CHECK_EQ(101, info.wID);
	TEST_CHECK_EQ(MF_CHECKED | MF_GRAYED, info.fState & (MF_CHECKED | MF_GRAYED));
	TEST_CHECK_EQ(5, info.cch);
	info.dwTypeData = text;
	info.cch = 3;
	TEST_CHECK(GetMenuItemInfoW(menu, 0, TRUE, &info));
	TEST_CHECK_EQ(2, info.cch);
	TEST_CHECK_EQ(L'A', text[0]);
	TEST_CHECK_EQ(L'l', text[1]);
	TEST_CHECK_EQ(0, text[2]);

	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = MIIM_ID | MIIM_STRING;
	info.wID = 303;
	info.dwTypeData = L"End";
	TEST_CHECK(InsertMenuItemW(menu, (UINT)-1, TRUE, &info));
	TEST_CHECK_EQ(3, GetMenuItemCount(menu));
	info.wID = 304;
	info.dwTypeData = L"Changed";
	TEST_CHECK(SetMenuItemInfoW(menu, 2, TRUE, &info));
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = MIIM_ID | MIIM_STRING;
	TEST_CHECK(GetMenuItemInfoW(menu, 2, TRUE, &info));
	TEST_CHECK_EQ(304, info.wID);
	TEST_CHECK_EQ(7, info.cch);

	TEST_CHECK(DeleteMenu(menu, 1, MF_BYPOSITION));
	TEST_CHECK_EQ(-1, GetMenuItemCount(popup));
	TEST_CHECK_EQ(2, GetMenuItemCount(menu));
	TEST_CHECK(DestroyMenu(menu));
	TEST_CHECK_EQ(-1, GetMenuItemCount(menu));
	SetLastError(sentinel);
	TEST_CHECK(!DestroyMenu(menu));
	TEST_CHECK_EQ(sentinel, GetLastError());

	menu = CreateMenu();
	ZeroMemory(&info, sizeof(info));
	info.cbSize = 1;
	SetLastError(sentinel);
	TEST_CHECK(!InsertMenuItemW(menu, 0, TRUE, &info));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	info.cbSize = sizeof(info);
	info.fMask = MIIM_ID;
	SetLastError(sentinel);
	TEST_CHECK(!GetMenuItemInfoW(menu, 0, TRUE, &info));
	TEST_CHECK_EQ(ERROR_MENU_ITEM_NOT_FOUND, GetLastError());
	TEST_CHECK(DestroyMenu(menu));
	return 0;
}
