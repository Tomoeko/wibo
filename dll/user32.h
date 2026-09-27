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
HWINSTA WINAPI GetProcessWindowStation();
HANDLE WINAPI GetThreadDesktop(DWORD dwThreadId);
BOOL WINAPI GetUserObjectInformationA(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded);
HWND WINAPI GetActiveWindow();
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
