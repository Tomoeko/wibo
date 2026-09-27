#pragma once

#include "types.h"

struct COORD {
	SHORT X;
	SHORT Y;
};

struct SMALL_RECT {
	SHORT Left;
	SHORT Top;
	SHORT Right;
	SHORT Bottom;
};

struct CONSOLE_SCREEN_BUFFER_INFO {
	COORD dwSize;
	COORD dwCursorPosition;
	WORD wAttributes;
	SMALL_RECT srWindow;
	COORD dwMaximumWindowSize;
};

struct INPUT_RECORD;

struct CONSOLE_READCONSOLE_CONTROL {
	DWORD nLength;
	DWORD nInitialChars;
	DWORD dwCtrlWakeupMask;
	DWORD dwControlKeyState;
};
static_assert(sizeof(CONSOLE_READCONSOLE_CONTROL) == 16);

typedef BOOL(_CC_STDCALL *PHANDLER_ROUTINE)(DWORD CtrlType);

namespace kernel32 {

BOOL WINAPI AttachConsole(DWORD processId);
BOOL WINAPI GetConsoleMode(HANDLE hConsoleHandle, LPDWORD lpMode);
BOOL WINAPI SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode);
UINT WINAPI GetConsoleCP();
UINT WINAPI GetConsoleOutputCP();
BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add);
bool isConsoleControlCIgnored();
void initializeConsoleControlCIgnore(bool ignore);
BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE hConsoleOutput, CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo);
BOOL WINAPI SetConsoleTextAttribute(HANDLE hConsoleOutput, WORD wAttributes);
BOOL WINAPI WriteConsoleW(HANDLE hConsoleOutput, LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite,
						  LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
BOOL WINAPI ReadConsoleW(HANDLE hConsoleInput, LPVOID buffer, DWORD charsToRead, LPDWORD charsRead, LPVOID control);
DWORD WINAPI GetConsoleTitleA(LPSTR lpConsoleTitle, DWORD nSize);
DWORD WINAPI GetConsoleTitleW(LPWSTR lpConsoleTitle, DWORD nSize);
BOOL WINAPI PeekConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead);
BOOL WINAPI ReadConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead);
BOOL WINAPI VerifyConsoleIoHandle(HANDLE handle);

} // namespace kernel32
