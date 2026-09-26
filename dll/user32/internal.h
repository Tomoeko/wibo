#pragma once

#include "user32.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace user32::detail {

bool ghostingDisabled();

struct WindowClass {
	WNDCLASSEXW definition{};
	ATOM atom = 0;
	std::u16string name;
	std::vector<uint8_t> extra;
	std::atomic<unsigned> windows = 0;
	~WindowClass();
};

std::shared_ptr<WindowClass> findWindowClass(HINSTANCE instance, LPCWSTR name);

} // namespace user32::detail
