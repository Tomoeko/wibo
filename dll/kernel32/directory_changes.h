#pragma once
#include "internal.h"
#include "minwinbase.h"
namespace kernel32 {
void cancelDirectoryIoForThread(pthread_t thread);
bool cancelDirectoryChanges(DirectoryObject &directory, OVERLAPPED *operation);
bool cancelDirectoryChangesForThread(DirectoryObject &directory, pthread_t thread);
} // namespace kernel32
