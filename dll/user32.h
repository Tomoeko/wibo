#pragma once

#include "types.h"

namespace user32 {

struct WNDCLASSEXW {
	UINT cbSize;
	UINT style;
	GUEST_PTR lpfnWndProc;
	int cbClsExtra;
	int cbWndExtra;
	HINSTANCE hInstance;
	HANDLE hIcon;
	HANDLE hCursor;
	HANDLE hbrBackground;
	GUEST_PTR lpszMenuName;
	GUEST_PTR lpszClassName;
	HANDLE hIconSm;
};
#ifdef WIBO_GUEST_64
static_assert(sizeof(WNDCLASSEXW) == 80);
#else
static_assert(sizeof(WNDCLASSEXW) == 48);
#endif

struct MENUITEMINFOW {
	UINT cbSize;
	UINT fMask;
	UINT fType;
	UINT fState;
	UINT wID;
	HANDLE hSubMenu;
	HANDLE hbmpChecked;
	HANDLE hbmpUnchecked;
	ULONG_PTR dwItemData;
	GUEST_PTR dwTypeData;
	UINT cch;
	HANDLE hbmpItem;
};
#ifdef WIBO_GUEST_64
static_assert(sizeof(MENUITEMINFOW) == 80);
#else
static_assert(sizeof(MENUITEMINFOW) == 48);
#endif

struct ACCEL {
	BYTE fVirt;
	WORD key;
	WORD cmd;
};
static_assert(sizeof(ACCEL) == 6);

struct MSG {
	HWND hwnd;
	UINT message;
	UINT_PTR wParam;
	LONG_PTR lParam;
	DWORD time;
	LONG pointX;
	LONG pointY;
	DWORD privateData;
};
#ifdef WIBO_GUEST_64
static_assert(sizeof(MSG) == 48);
#else
static_assert(sizeof(MSG) == 32);
#endif

HANDLE WINAPI CreateAcceleratorTableA(const ACCEL *entries, int count);
HANDLE WINAPI CreateAcceleratorTableW(const ACCEL *entries, int count);
BOOL WINAPI DestroyAcceleratorTable(HANDLE table);
int WINAPI CopyAcceleratorTableA(HANDLE table, ACCEL *entries, int count);
int WINAPI CopyAcceleratorTableW(HANDLE table, ACCEL *entries, int count);
int WINAPI TranslateAcceleratorA(HWND window, HANDLE table, const MSG *message);
int WINAPI TranslateAcceleratorW(HWND window, HANDLE table, const MSG *message);

HANDLE WINAPI CreateMenu();
HANDLE WINAPI CreatePopupMenu();
BOOL WINAPI DestroyMenu(HANDLE menu);
BOOL WINAPI AppendMenuA(HANDLE menu, UINT flags, UINT_PTR item, LPCSTR text);
BOOL WINAPI DeleteMenu(HANDLE menu, UINT item, UINT flags);
int WINAPI GetMenuItemCount(HANDLE menu);
HANDLE WINAPI GetSubMenu(HANDLE menu, int position);
DWORD WINAPI CheckMenuItem(HANDLE menu, UINT item, UINT flags);
DWORD WINAPI EnableMenuItem(HANDLE menu, UINT item, UINT flags);
BOOL WINAPI InsertMenuItemW(HANDLE menu, UINT item, BOOL byPosition, const MENUITEMINFOW *info);
BOOL WINAPI GetMenuItemInfoW(HANDLE menu, UINT item, BOOL byPosition, MENUITEMINFOW *info);
BOOL WINAPI SetMenuItemInfoW(HANDLE menu, UINT item, BOOL byPosition, const MENUITEMINFOW *info);
HANDLE WINAPI GetMenu(HWND window);
BOOL WINAPI SetMenu(HWND window, HANDLE menu);
BOOL WINAPI DrawMenuBar(HWND window);
BOOL WINAPI TrackPopupMenuEx(HANDLE menu, UINT flags, int x, int y, HWND window, const void *parameters);

ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *definition);
BOOL WINAPI GetClassInfoExW(HINSTANCE instance, LPCWSTR name, WNDCLASSEXW *definition);
BOOL WINAPI UnregisterClassW(LPCWSTR name, HINSTANCE instance);

void WINAPI DisableProcessWindowsGhosting();
BOOL WINAPI IsCharAlphaW(WCHAR character);
LPSTR WINAPI CharNextExA(WORD codePage, LPCSTR current, DWORD flags);
DWORD WINAPI CharUpperBuffW(LPWSTR buffer, DWORD length);
#ifdef WIBO_GUEST_64
int CDECL_NO_CONV wsprintfW(LPWSTR buffer, LPCWSTR format, ...);
#else
// The trampoline supplies the cursor immediately after the named guest arguments.
int CDECL wsprintfW(LPWSTR buffer, LPCWSTR format, const void *arguments) WIBO_ANNOTATE("GUEST_STACK_VARARGS");
#endif
int WINAPI GetSystemMetrics(int index);
int WINAPI LoadStringA(HMODULE hInstance, UINT uID, LPSTR lpBuffer, int cchBufferMax);
int WINAPI LoadStringW(HMODULE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax);
int WINAPI MessageBoxA(HWND hwnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
HKL WINAPI GetKeyboardLayout(DWORD idThread);
BOOL WINAPI GetKeyboardLayoutNameW(LPWSTR name);
SHORT WINAPI VkKeyScanExW(WCHAR character, HKL requested);
UINT WINAPI MapVirtualKeyA(UINT code, UINT type);
UINT WINAPI MapVirtualKeyW(UINT code, UINT type);
UINT WINAPI MapVirtualKeyExA(UINT code, UINT type, HKL layout);
int WINAPI ToUnicode(UINT key, UINT scan, const BYTE *state, LPWSTR output, int capacity, UINT flags);
HWINSTA WINAPI GetProcessWindowStation();
HANDLE WINAPI GetThreadDesktop(DWORD dwThreadId);
BOOL WINAPI GetUserObjectInformationA(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded);
HWND WINAPI GetActiveWindow();
HWND WINAPI GetForegroundWindow();
HWND WINAPI GetFocus();
HWND WINAPI SetFocus(HWND window);
HWND WINAPI SetActiveWindow(HWND window);
BOOL WINAPI SetForegroundWindow(HWND window);
BOOL WINAPI AllowSetForegroundWindow(DWORD processId);
DWORD WINAPI GetWindowThreadProcessId(HWND window, LPDWORD processId);
BOOL WINAPI IsWindow(HWND window);
BOOL WINAPI IsWindowVisible(HWND window);
BOOL WINAPI IsWindowEnabled(HWND window);
DWORD WINAPI GetSysColor(int nIndex);
UINT WINAPI RegisterWindowMessageA(LPCSTR lpString);
UINT WINAPI RegisterWindowMessageW(LPCWSTR lpString);
UINT WINAPI RegisterClipboardFormatA(LPCSTR name);
UINT WINAPI RegisterClipboardFormatW(LPCWSTR name);
HANDLE WINAPI LoadCursorA(HINSTANCE instance, LPCSTR name);
HANDLE WINAPI LoadCursorW(HINSTANCE instance, LPCWSTR name);
HANDLE WINAPI SetCursor(HANDLE cursor);
HANDLE WINAPI LoadIconA(HINSTANCE instance, LPCSTR name);
HANDLE WINAPI LoadIconW(HINSTANCE instance, LPCWSTR name);

} // namespace user32
