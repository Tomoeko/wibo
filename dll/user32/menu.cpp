#include "user32.h"

#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr UINT MF_GRAYED = 0x0001;
constexpr UINT MF_DISABLED = 0x0002;
constexpr UINT MF_CHECKED = 0x0008;
constexpr UINT MF_POPUP = 0x0010;
constexpr UINT MF_MENUBARBREAK = 0x0020;
constexpr UINT MF_MENUBREAK = 0x0040;
constexpr UINT MF_OWNERDRAW = 0x0100;
constexpr UINT MF_BYPOSITION = 0x0400;
constexpr UINT MF_SEPARATOR = 0x0800;
constexpr UINT MFT_SEPARATOR = 0x0800;
constexpr UINT MIIM_STATE = 0x0001;
constexpr UINT MIIM_ID = 0x0002;
constexpr UINT MIIM_SUBMENU = 0x0004;
constexpr UINT MIIM_CHECKMARKS = 0x0008;
constexpr UINT MIIM_TYPE = 0x0010;
constexpr UINT MIIM_DATA = 0x0020;
constexpr UINT MIIM_STRING = 0x0040;
constexpr UINT MIIM_BITMAP = 0x0080;
constexpr UINT MIIM_FTYPE = 0x0100;
constexpr UINT kInfoMask = MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_CHECKMARKS | MIIM_TYPE | MIIM_DATA |
					 MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;

struct MenuItem {
	UINT type = 0;
	UINT state = 0;
	UINT id = 0;
	HANDLE submenu = 0;
	HANDLE checkedBitmap = 0;
	HANDLE uncheckedBitmap = 0;
	HANDLE bitmap = 0;
	ULONG_PTR data = 0;
	std::u16string text;
};

struct Menu {
	bool popup = false;
	std::vector<MenuItem> items;
};

struct MenuRegistry {
	std::mutex mutex;
	std::unordered_map<HANDLE, Menu> menus;
	HANDLE nextHandle = 0x20000;
};

MenuRegistry &registry() {
	static MenuRegistry value;
	return value;
}

bool submenuReachesLocked(MenuRegistry &state, HANDLE start, HANDLE target,
							std::unordered_set<HANDLE> &visited) {
	if (start == target)
		return true;
	if (!visited.insert(start).second)
		return false;
	auto found = state.menus.find(start);
	if (found == state.menus.end())
		return false;
	for (const MenuItem &item : found->second.items)
		if (item.submenu && submenuReachesLocked(state, item.submenu, target, visited))
			return true;
	return false;
}

bool submenuReachesLocked(MenuRegistry &state, HANDLE start, HANDLE target) {
	std::unordered_set<HANDLE> visited;
	return submenuReachesLocked(state, start, target, visited);
}

void destroyMenuLocked(MenuRegistry &state, HANDLE menu, std::unordered_set<HANDLE> &visited) {
	if (!visited.insert(menu).second)
		return;
	auto found = state.menus.find(menu);
	if (found == state.menus.end())
		return;
	std::vector<HANDLE> children;
	for (const MenuItem &item : found->second.items)
		if (item.submenu)
			children.push_back(item.submenu);
	state.menus.erase(found);
	for (HANDLE child : children)
		destroyMenuLocked(state, child, visited);
}

MenuItem *findItemLocked(MenuRegistry &state, HANDLE menu, UINT key, bool byPosition,
							std::unordered_set<HANDLE> &visited, HANDLE *owner) {
	if (!visited.insert(menu).second)
		return nullptr;
	auto found = state.menus.find(menu);
	if (found == state.menus.end())
		return nullptr;
	auto &items = found->second.items;
	if (byPosition) {
		if (key >= items.size())
			return nullptr;
		if (owner)
			*owner = menu;
		return &items[key];
	}
	for (auto &item : items)
		if (item.id == key) {
			if (owner)
				*owner = menu;
			return &item;
		}
	for (auto &item : items)
		if (item.submenu)
			if (MenuItem *nested = findItemLocked(state, item.submenu, key, false, visited, owner))
				return nested;
	return nullptr;
}

MenuItem *findItemLocked(MenuRegistry &state, HANDLE menu, UINT key, bool byPosition, HANDLE *owner = nullptr) {
	std::unordered_set<HANDLE> visited;
	return findItemLocked(state, menu, key, byPosition, visited, owner);
}

bool readWideText(GUEST_PTR pointer, std::u16string &text) {
	text.clear();
	if (!pointer)
		return true;
	const WCHAR *source = fromGuestPtr<WCHAR>(pointer);
	const size_t length = wstrnlen(source, 32768);
	if (length == 32768) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	text.reserve(length);
	for (size_t i = 0; i < length; ++i)
		text.push_back(static_cast<char16_t>(source[i]));
	return true;
}

bool readAnsiText(LPCSTR source, std::u16string &text) {
	text.clear();
	if (!source)
		return true;
	const size_t length = strnlen(source, 32768);
	if (length == 32768) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	text.reserve(length);
	for (size_t i = 0; i < length; ++i) {
		const unsigned char ch = static_cast<unsigned char>(source[i]);
		if (ch >= 0x80) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		text.push_back(ch);
	}
	return true;
}

bool applyInfoLocked(MenuRegistry &state, HANDLE owner, MenuItem &item, const user32::MENUITEMINFOW &info) {
	if (info.fMask & ~kInfoMask) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return false;
	}
	if (info.fMask & MIIM_SUBMENU) {
		if (info.hSubMenu && !state.menus.contains(info.hSubMenu)) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return false;
		}
		if (info.hSubMenu && submenuReachesLocked(state, info.hSubMenu, owner))
			return false;
	}
	if (info.fMask & (MIIM_FTYPE | MIIM_TYPE)) {
		if (info.fType & ~(MF_MENUBARBREAK | MF_MENUBREAK | MF_OWNERDRAW | MFT_SEPARATOR)) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		if (info.fType & MF_OWNERDRAW) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
	}
	std::u16string newText;
	const UINT effectiveType = (info.fMask & (MIIM_FTYPE | MIIM_TYPE)) ? info.fType : item.type;
	const bool hasString = (info.fMask & (MIIM_STRING | MIIM_TYPE)) && !(effectiveType & MFT_SEPARATOR);
	if (hasString && !readWideText(info.dwTypeData, newText))
		return false;
	if (info.fMask & (MIIM_FTYPE | MIIM_TYPE))
		item.type = info.fType;
	if (info.fMask & MIIM_STATE)
		item.state = info.fState;
	if (info.fMask & MIIM_ID)
		item.id = info.wID;
	if (info.fMask & MIIM_SUBMENU)
		item.submenu = info.hSubMenu;
	if (info.fMask & MIIM_CHECKMARKS) {
		item.checkedBitmap = info.hbmpChecked;
		item.uncheckedBitmap = info.hbmpUnchecked;
	}
	if (info.fMask & MIIM_BITMAP)
		item.bitmap = info.hbmpItem;
	if (info.fMask & MIIM_DATA)
		item.data = info.dwItemData;
	if (info.fMask & (MIIM_STRING | MIIM_TYPE))
		item.text = std::move(newText);
	return true;
}

bool validInfo(const user32::MENUITEMINFOW *info) {
	if (info && info->cbSize == sizeof(*info))
		return true;
	kernel32::setLastError(ERROR_INVALID_PARAMETER);
	return false;
}

} // namespace

namespace user32 {

HANDLE WINAPI CreateMenu() {
	HOST_CONTEXT_GUARD();
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	if (state.nextHandle > std::numeric_limits<int>::max() - 4) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	const HANDLE handle = state.nextHandle;
	state.nextHandle += 4;
	state.menus.emplace(handle, Menu{});
	return handle;
}

HANDLE WINAPI CreatePopupMenu() {
	HOST_CONTEXT_GUARD();
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	if (state.nextHandle > std::numeric_limits<int>::max() - 4) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	const HANDLE handle = state.nextHandle;
	state.nextHandle += 4;
	state.menus.emplace(handle, Menu{true, {}});
	return handle;
}

BOOL WINAPI DestroyMenu(HANDLE menu) {
	HOST_CONTEXT_GUARD();
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	if (!state.menus.contains(menu))
		return FALSE;
	std::unordered_set<HANDLE> visited;
	destroyMenuLocked(state, menu, visited);
	return TRUE;
}

BOOL WINAPI AppendMenuA(HANDLE menu, UINT flags, UINT_PTR item, LPCSTR text) {
	HOST_CONTEXT_GUARD();
	if (flags & ~(MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_POPUP | MF_MENUBARBREAK | MF_MENUBREAK |
					 MF_SEPARATOR)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	MenuItem entry;
	entry.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED);
	entry.type = flags & (MF_MENUBARBREAK | MF_MENUBREAK | MF_SEPARATOR);
	if (flags & MF_POPUP) {
		entry.submenu = static_cast<HANDLE>(item);
		entry.id = static_cast<UINT>(item);
	} else {
		entry.id = static_cast<UINT>(item);
	}
	if (!(flags & MF_SEPARATOR) && !readAnsiText(text, entry.text))
		return FALSE;
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	auto found = state.menus.find(menu);
	if (found == state.menus.end())
		return FALSE;
	if (entry.submenu && !state.menus.contains(entry.submenu)) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (entry.submenu && submenuReachesLocked(state, entry.submenu, menu))
		return FALSE;
	found->second.items.push_back(std::move(entry));
	return TRUE;
}

BOOL WINAPI DeleteMenu(HANDLE menu, UINT item, UINT flags) {
	HOST_CONTEXT_GUARD();
	if (flags & ~MF_BYPOSITION) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	auto found = state.menus.find(menu);
	if (found == state.menus.end())
		return FALSE;
	auto &items = found->second.items;
	const auto match = (flags & MF_BYPOSITION)
		? (item < items.size() ? items.begin() + item : items.end())
		: std::find_if(items.begin(), items.end(), [item](const MenuItem &entry) { return entry.id == item; });
	if (match == items.end())
		return FALSE;
	const HANDLE submenu = match->submenu;
	items.erase(match);
	if (submenu) {
		std::unordered_set<HANDLE> visited;
		destroyMenuLocked(state, submenu, visited);
	}
	return TRUE;
}

int WINAPI GetMenuItemCount(HANDLE menu) {
	HOST_CONTEXT_GUARD();
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	auto found = state.menus.find(menu);
	return found == state.menus.end() ? -1 : static_cast<int>(found->second.items.size());
}

HANDLE WINAPI GetSubMenu(HANDLE menu, int position) {
	HOST_CONTEXT_GUARD();
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	auto found = state.menus.find(menu);
	if (found == state.menus.end() || position < 0 || size_t(position) >= found->second.items.size())
		return 0;
	return found->second.items[position].submenu;
}

DWORD WINAPI CheckMenuItem(HANDLE menu, UINT item, UINT flags) {
	HOST_CONTEXT_GUARD();
	if (flags & ~(MF_BYPOSITION | MF_CHECKED)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return UINT(-1);
	}
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	MenuItem *entry = findItemLocked(state, menu, item, (flags & MF_BYPOSITION) != 0);
	if (!entry)
		return UINT(-1);
	const UINT previous = entry->state & MF_CHECKED;
	entry->state = (entry->state & ~MF_CHECKED) | (flags & MF_CHECKED);
	return previous;
}

DWORD WINAPI EnableMenuItem(HANDLE menu, UINT item, UINT flags) {
	HOST_CONTEXT_GUARD();
	if (flags & ~(MF_BYPOSITION | MF_GRAYED | MF_DISABLED)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return UINT(-1);
	}
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	MenuItem *entry = findItemLocked(state, menu, item, (flags & MF_BYPOSITION) != 0);
	if (!entry)
		return UINT(-1);
	const UINT previous = entry->state & (MF_GRAYED | MF_DISABLED);
	entry->state = (entry->state & ~(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
	return previous;
}

BOOL WINAPI InsertMenuItemW(HANDLE menu, UINT item, BOOL byPosition, const MENUITEMINFOW *info) {
	HOST_CONTEXT_GUARD();
	if (!validInfo(info))
		return FALSE;
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	auto found = state.menus.find(menu);
	if (found == state.menus.end())
		return FALSE;
	MenuItem entry;
	if (!applyInfoLocked(state, menu, entry, *info))
		return FALSE;
	auto &items = found->second.items;
	size_t position = items.size();
	if (byPosition) {
		if (item != UINT(-1))
			position = std::min<size_t>(item, position);
	} else {
		const auto match = std::find_if(items.begin(), items.end(), [item](const MenuItem &current) {
			return current.id == item;
		});
		if (match != items.end())
			position = static_cast<size_t>(match - items.begin());
	}
	items.insert(items.begin() + position, std::move(entry));
	return TRUE;
}

BOOL WINAPI GetMenuItemInfoW(HANDLE menu, UINT item, BOOL byPosition, MENUITEMINFOW *info) {
	HOST_CONTEXT_GUARD();
	if (!validInfo(info))
		return FALSE;
	if (info->fMask & ~kInfoMask) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	MenuItem *entry = findItemLocked(state, menu, item, byPosition);
	if (!entry) {
		kernel32::setLastError(ERROR_MENU_ITEM_NOT_FOUND);
		return FALSE;
	}
	if (info->fMask & (MIIM_TYPE | MIIM_FTYPE))
		info->fType = entry->type;
	if (info->fMask & MIIM_STATE)
		info->fState = entry->state;
	if (info->fMask & MIIM_ID)
		info->wID = entry->id;
	if (info->fMask & MIIM_SUBMENU)
		info->hSubMenu = entry->submenu;
	if (info->fMask & MIIM_CHECKMARKS) {
		info->hbmpChecked = entry->checkedBitmap;
		info->hbmpUnchecked = entry->uncheckedBitmap;
	}
	if (info->fMask & MIIM_DATA)
		info->dwItemData = entry->data;
	if (info->fMask & MIIM_BITMAP)
		info->hbmpItem = entry->bitmap;
	if ((info->fMask & (MIIM_STRING | MIIM_TYPE)) && !(entry->type & MFT_SEPARATOR)) {
		const UINT length = static_cast<UINT>(entry->text.size());
		if (!info->dwTypeData)
			info->cch = length;
		else {
			WCHAR *destination = fromGuestPtr<WCHAR>(info->dwTypeData);
			const UINT copied = info->cch ? std::min(length, info->cch - 1) : 0;
			for (UINT i = 0; i < copied; ++i)
				destination[i] = static_cast<WCHAR>(entry->text[i]);
			if (info->cch)
				destination[copied] = 0;
			info->cch = copied;
		}
	}
	return TRUE;
}

BOOL WINAPI SetMenuItemInfoW(HANDLE menu, UINT item, BOOL byPosition, const MENUITEMINFOW *info) {
	HOST_CONTEXT_GUARD();
	if (!validInfo(info))
		return FALSE;
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	HANDLE owner = 0;
	MenuItem *entry = findItemLocked(state, menu, item, byPosition, &owner);
	if (!entry)
		return FALSE;
	return applyInfoLocked(state, owner, *entry, *info) ? TRUE : FALSE;
}

HANDLE WINAPI GetMenu(HWND window) {
	HOST_CONTEXT_GUARD();
	(void)window;
	kernel32::setLastError(ERROR_INVALID_WINDOW_HANDLE);
	return 0;
}

BOOL WINAPI SetMenu(HWND window, HANDLE menu) {
	HOST_CONTEXT_GUARD();
	(void)window;
	(void)menu;
	kernel32::setLastError(ERROR_INVALID_WINDOW_HANDLE);
	return FALSE;
}

BOOL WINAPI DrawMenuBar(HWND window) {
	HOST_CONTEXT_GUARD();
	if (window)
		kernel32::setLastError(ERROR_INVALID_WINDOW_HANDLE);
	return FALSE;
}

BOOL WINAPI TrackPopupMenuEx(HANDLE menu, UINT flags, int x, int y, HWND window, const void *parameters) {
	HOST_CONTEXT_GUARD();
	(void)flags;
	(void)x;
	(void)y;
	(void)window;
	(void)parameters;
	auto &state = registry();
	std::lock_guard lock(state.mutex);
	if (!state.menus.contains(menu)) {
		kernel32::setLastError(ERROR_INVALID_MENU_HANDLE);
		return FALSE;
	}
	kernel32::setLastError(ERROR_INVALID_WINDOW_HANDLE);
	return FALSE;
}

} // namespace user32
