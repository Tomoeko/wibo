#include "user32.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"
#include "system_provider.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {

struct BitmapData {
	uint32_t width = 0, height = 0, stride = 0, planes = 0, bitsPerPixel = 0;
	std::vector<uint8_t> bytes;
};

struct ImageObject : ObjectBase {
	static constexpr ObjectType kType = ObjectType::UserImage;
	bool icon = false;
	uint32_t hotspotX = 0, hotspotY = 0;
	BitmapData mask, color;
	ImageObject() : ObjectBase(kType) {}
};

std::mutex g_imageMutex;
std::map<std::vector<std::string>, HANDLE> g_sharedImages;
thread_local HANDLE g_currentCursor = NO_HANDLE;

bool readBitmap(wibo::provider::Reader &reader, BitmapData &bitmap) {
	if (!reader.number(bitmap.width) || !reader.number(bitmap.height) || !reader.number(bitmap.stride) ||
		!reader.number(bitmap.planes) || !reader.number(bitmap.bitsPerPixel) || !reader.bytes(bitmap.bytes))
		return false;
	if (!bitmap.width || bitmap.width > 4096 || !bitmap.height || bitmap.height > 8192 || bitmap.planes != 1)
		return false;
	switch (bitmap.bitsPerPixel) {
	case 1:
	case 4:
	case 8:
	case 16:
	case 24:
	case 32:
		break;
	default:
		return false;
	}
	const uint32_t rowSize = ((bitmap.width * bitmap.bitsPerPixel + 15) / 16) * 2;
	return bitmap.stride == rowSize && uint64_t(rowSize) * bitmap.height == bitmap.bytes.size();
}

HANDLE loadImageResource(bool icon, HINSTANCE instance, const wibo::ResourceIdentifier &name) {
	std::string image, encoded;
	if (instance) {
		const auto *module = wibo::moduleInfoFromHandle(instance);
		if (!module || module->resolvedPath.empty()) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return NO_HANDLE;
		}
		image = files::pathToWindows(module->resolvedPath);
	}
	if (name.isString && !wibo::provider::encodeUtf8(name.name, encoded)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	std::vector<std::string> arguments = {"image-load", icon ? "icon" : "cursor", image, name.isString ? "name" : "id",
										  name.isString ? encoded : std::to_string(name.id)};
	// Shared resource handles and their copied bitmap data remain resident.
	std::lock_guard lock(g_imageMutex);
	if (const auto found = g_sharedImages.find(arguments); found != g_sharedImages.end())
		return found->second;
	std::vector<uint8_t> response;
	if (!wibo::provider::request(arguments, response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return NO_HANDLE;
	}
	if (status) {
		kernel32::setLastError(reader.done() ? status : ERROR_INVALID_DATA);
		return NO_HANDLE;
	}
	auto cursor = make_pin<ImageObject>();
	uint32_t hasColor = 0, isIcon = 0;
	if (!reader.number(isIcon) || isIcon != uint32_t(icon) || !reader.number(cursor->hotspotX) ||
		!reader.number(cursor->hotspotY) || !readBitmap(reader, cursor->mask) || !reader.number(hasColor) ||
		hasColor > 1 || (hasColor && !readBitmap(reader, cursor->color)) || !reader.done()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return NO_HANDLE;
	}
	const auto &mask = cursor->mask;
	const auto &color = cursor->color;
	const uint32_t height = hasColor ? color.height : mask.height / 2;
	if (mask.bitsPerPixel != 1 || (!hasColor && mask.height % 2) ||
		(hasColor && (color.width != mask.width || color.height != mask.height)) || cursor->hotspotX >= mask.width ||
		cursor->hotspotY >= height) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return NO_HANDLE;
	}
	cursor->icon = icon;
	const HANDLE handle = wibo::handles().alloc(std::move(cursor), 0, 0);
	if (!handle) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NO_HANDLE;
	}
	g_sharedImages.emplace(std::move(arguments), handle);
	return handle;
}

} // namespace

namespace user32 {

HANDLE WINAPI SetCursor(HANDLE cursor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetCursor(%p)\n", cursor);
	const char *headless = std::getenv("WIBO_HEADLESS");
	if (!headless || std::strcmp(headless, "1") != 0) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	const auto image = cursor ? wibo::handles().getAs<ImageObject>(cursor) : Pin<ImageObject>();
	if (cursor && (!image || image->icon)) {
		kernel32::setLastError(1402); // ERROR_INVALID_CURSOR_HANDLE
		return NO_HANDLE;
	}
	// A headless input queue maintains cursor state without desktop presentation.
	const HANDLE previous = g_currentCursor;
	g_currentCursor = cursor;
	return previous;
}

HANDLE WINAPI LoadCursorA(HINSTANCE instance, LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadCursorA(%p, %p)\n", instance, name);
	return loadImageResource(false, instance, wibo::resourceIdentifierFromAnsi(name));
}

HANDLE WINAPI LoadCursorW(HINSTANCE instance, LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadCursorW(%p, %p)\n", instance, name);
	return loadImageResource(false, instance, wibo::resourceIdentifierFromWide(name));
}

HANDLE WINAPI LoadIconA(HINSTANCE instance, LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadIconA(%p, %p)\n", instance, name);
	return loadImageResource(true, instance, wibo::resourceIdentifierFromAnsi(name));
}

HANDLE WINAPI LoadIconW(HINSTANCE instance, LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadIconW(%p, %p)\n", instance, name);
	return loadImageResource(true, instance, wibo::resourceIdentifierFromWide(name));
}

} // namespace user32
