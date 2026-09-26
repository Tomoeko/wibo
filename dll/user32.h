#pragma once

#include "types.h"

namespace user32 {

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
