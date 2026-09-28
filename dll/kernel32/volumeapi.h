#pragma once

#include "types.h"

namespace kernel32 {
BOOL WINAPI GetVolumeInformationByHandleW(HANDLE file, LPWSTR volumeName, DWORD volumeNameSize, LPDWORD serialNumber,
										  LPDWORD maximumComponentLength, LPDWORD fileSystemFlags,
										  LPWSTR fileSystemName, DWORD fileSystemNameSize);
}
