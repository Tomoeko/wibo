#pragma once

#include "types.h"

namespace combase {

enum RO_INIT_TYPE {
	RO_INIT_SINGLETHREADED = 0,
	RO_INIT_MULTITHREADED = 1,
};

HRESULT WINAPI RoInitialize(RO_INIT_TYPE initType);
void WINAPI RoUninitialize();

} // namespace combase
