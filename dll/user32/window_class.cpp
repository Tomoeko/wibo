#include "user32/internal.h"

#include "context.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {

constexpr DWORD ERROR_CLASS_ALREADY_EXISTS = 1410;
constexpr DWORD ERROR_CLASS_DOES_NOT_EXIST = 1411;
constexpr DWORD ERROR_CLASS_HAS_WINDOWS = 1412;
constexpr UINT CS_GLOBALCLASS = 0x4000;
constexpr size_t kMaxClassExtra = 1024 * 1024;

struct ClassRegistry {
	std::mutex mutex;
	std::vector<std::shared_ptr<user32::detail::WindowClass>> classes;
	uint32_t nextAtom = 0xC000;
};

ClassRegistry &classRegistry() {
	static ClassRegistry registry;
	return registry;
}

std::u16string foldedName(LPCWSTR name) {
	std::u16string result;
	for (; *name; ++name)
		result.push_back(static_cast<char16_t>(wcharToLower(*name)));
	return result;
}

bool matchesName(const user32::detail::WindowClass &registered, LPCWSTR name) {
	if (uintptr_t(name) <= 0xFFFF)
		return registered.atom == uintptr_t(name);
	return registered.name == foldedName(name);
}

std::shared_ptr<user32::detail::WindowClass> findClassLocked(ClassRegistry &registry, HINSTANCE instance,
															 LPCWSTR name) {
	for (const auto &registered : registry.classes)
		if (registered->definition.hInstance == instance && matchesName(*registered, name))
			return registered;
	for (const auto &registered : registry.classes)
		if ((registered->definition.style & CS_GLOBALCLASS) && matchesName(*registered, name))
			return registered;
	return nullptr;
}

GUEST_PTR copyName(LPCWSTR source, size_t length) {
	const size_t size = (length + 1) * sizeof(WCHAR);
	void *data = wibo::heap::guestMalloc(size, false);
	if (!data)
		return GUEST_NULL;
	std::memcpy(data, source, size);
	return toGuestPtr(data);
}

} // namespace

namespace user32::detail {

WindowClass::~WindowClass() {
	if (definition.lpszClassName > 0xFFFF)
		wibo::heap::guestFree(fromGuestPtr(definition.lpszClassName));
	if (definition.lpszMenuName > 0xFFFF)
		wibo::heap::guestFree(fromGuestPtr(definition.lpszMenuName));
}

std::shared_ptr<WindowClass> findWindowClass(HINSTANCE instance, LPCWSTR name) {
	auto &registry = classRegistry();
	std::lock_guard lock(registry.mutex);
	return findClassLocked(registry, instance, name);
}

} // namespace user32::detail

namespace user32 {

ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *definition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterClassExW(%p)\n", definition);
	if (!definition || definition->cbSize != sizeof(*definition) || !definition->lpfnWndProc ||
		!definition->lpszClassName || definition->cbClsExtra < 0 || definition->cbWndExtra < 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (size_t(definition->cbClsExtra) > kMaxClassExtra) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	auto &registry = classRegistry();
	std::lock_guard lock(registry.mutex);
	LPCWSTR name = fromGuestPtr<WCHAR>(definition->lpszClassName);
	if (definition->lpszClassName <= 0xFFFF) {
		const auto found = findClassLocked(registry, definition->hInstance, name);
		if (!found) {
			kernel32::setLastError(ERROR_CLASS_DOES_NOT_EXIST);
			return 0;
		}
		name = fromGuestPtr<WCHAR>(found->definition.lpszClassName);
	}
	const size_t length = wstrnlen(name, 257);
	if (!length || length > 256) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const auto folded = foldedName(name);
	ATOM atom = 0;
	for (const auto &registered : registry.classes) {
		if (registered->name != folded)
			continue;
		if (registered->definition.hInstance == definition->hInstance ||
			((registered->definition.style & definition->style) & CS_GLOBALCLASS)) {
			kernel32::setLastError(ERROR_CLASS_ALREADY_EXISTS);
			return 0;
		}
		atom = registered->atom;
	}
	if (!atom && registry.nextAtom > 0xFFFF) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	auto registered = std::make_shared<detail::WindowClass>();
	registered->definition = *definition;
	registered->definition.lpszClassName = GUEST_NULL;
	registered->definition.lpszMenuName = GUEST_NULL;
	registered->definition.lpszClassName = copyName(name, length);
	if (!registered->definition.lpszClassName) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (definition->lpszMenuName > 0xFFFF) {
		const auto *menu = fromGuestPtr<WCHAR>(definition->lpszMenuName);
		const size_t menuLength = wstrnlen(menu, 32768);
		if (menuLength >= 32768) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return 0;
		}
		registered->definition.lpszMenuName = copyName(menu, menuLength);
		if (!registered->definition.lpszMenuName) {
			kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return 0;
		}
	} else {
		registered->definition.lpszMenuName = definition->lpszMenuName;
	}
	registered->name = folded;
	registered->extra.resize(definition->cbClsExtra, 0);
	registered->atom = atom ? atom : static_cast<ATOM>(registry.nextAtom++);
	registry.classes.push_back(registered);
	return registered->atom;
}

BOOL WINAPI GetClassInfoExW(HINSTANCE instance, LPCWSTR name, WNDCLASSEXW *definition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetClassInfoExW(%p, %p, %p)\n", instance, name, definition);
	if (!name || !definition) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const auto registered = detail::findWindowClass(instance, name);
	if (!registered) {
		kernel32::setLastError(ERROR_CLASS_DOES_NOT_EXIST);
		return FALSE;
	}
	*definition = registered->definition;
	return TRUE;
}

BOOL WINAPI UnregisterClassW(LPCWSTR name, HINSTANCE instance) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UnregisterClassW(%p, %p)\n", name, instance);
	auto &registry = classRegistry();
	std::lock_guard lock(registry.mutex);
	const auto found = std::find_if(registry.classes.begin(), registry.classes.end(), [=](const auto &registered) {
		return registered->definition.hInstance == instance && matchesName(*registered, name);
	});
	if (found == registry.classes.end()) {
		kernel32::setLastError(ERROR_CLASS_DOES_NOT_EXIST);
		return FALSE;
	}
	if ((*found)->windows.load()) {
		kernel32::setLastError(ERROR_CLASS_HAS_WINDOWS);
		return FALSE;
	}
	registry.classes.erase(found);
	return TRUE;
}

} // namespace user32
