#pragma once

#include "types.h"

// VARIANT's record alternative contains two guest pointers, including on Win64.
struct AutomationVariant {
	WORD type;
	WORD reserved[3];
	union {
		ULONGLONG scalar;
		GUEST_PTR pointer;
		GUEST_PTR record[2];
	} value;
};
static_assert(offsetof(AutomationVariant, value) == 8);
static_assert(sizeof(AutomationVariant) == (sizeof(GUEST_PTR) == 8 ? 24 : 16));
