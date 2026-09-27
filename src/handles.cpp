#include "handles.h"
#include "errors.h"
#include "types.h"
#include <atomic>
#include <cassert>
#include <cstdint>

namespace {

constexpr uint32_t kHandleAlignShift = 2;
// Max index that still yields HANDLE < 0x8000 with (index + 1) << 2
constexpr uint32_t kCompatMaxIndex = (0x7FFFu >> kHandleAlignShift) - 1;
// Delay reuse of small handles to avoid accidental stale aliasing
constexpr uint32_t kQuarantineLen = 64;

inline uint32_t indexOf(HANDLE h) noexcept {
	uint32_t v = static_cast<uint32_t>(h);
	if (v == 0 || (v & ((1U << kHandleAlignShift) - 1)) != 0) {
		return UINT32_MAX;
	}
	return (v >> kHandleAlignShift) - 1;
}

inline HANDLE makeHandle(uint32_t index) noexcept {
	uint32_t v = (index + 1) << kHandleAlignShift;
	return static_cast<HANDLE>(v);
}

inline bool isPseudo(HANDLE h) noexcept { return static_cast<LONG_PTR>(h) < 0; }

} // namespace

Handles::~Handles() { clear(); }

void Handles::clear() {
	std::vector<Entry> entries;
	{
		std::unique_lock lock(m);
		entries.swap(mSlots);
		mFreeBelow.clear();
		mFreeAbove.clear();
		mQuarantine.clear();
		nextIndex = 0;
	}
	for (auto &entry : entries) {
		if (!entry.obj)
			continue;
		if (entry.obj->handleCount.fetch_sub(1, std::memory_order_relaxed) == 1) {
			entry.obj->onLastHandleClosed();
			if (mOnHandleZero)
				mOnHandleZero(entry.obj);
		}
		detail::deref(entry.obj);
	}
}

HANDLE Handles::alloc(Pin<> obj, uint32_t grantedAccess, uint32_t flags) {
	std::unique_lock lk(m);

	// Attempt to, in order:
	// 1) use a fresh index in the compat range (0..kCompatMaxIndex)
	// 2) reuse a recently-freed index in the compat range
	// 3) reuse a recently-freed index above the compat range
	// 4) use a fresh index above the compat range
	uint32_t idx;
	if (nextIndex <= kCompatMaxIndex) {
		idx = nextIndex++;
		if (idx >= mSlots.size()) {
			mSlots.emplace_back();
		}
	} else if (!mFreeBelow.empty()) {
		idx = mFreeBelow.back();
		mFreeBelow.pop_back();
	} else if (!mFreeAbove.empty()) {
		idx = mFreeAbove.back();
		mFreeAbove.pop_back();
	} else {
		idx = static_cast<uint32_t>(mSlots.size());
		mSlots.emplace_back();
	}

	// Initialize entry
	auto &e = mSlots[idx];
	e.obj = obj.release(); // Transfer ownership
	e.meta.grantedAccess = grantedAccess;
	e.meta.flags = flags;
	e.meta.typeCache = e.obj->type;
	if (e.meta.generation == 0) {
		e.meta.generation = 1;
	}

	HANDLE h = makeHandle(idx);
	e.obj->handleCount.fetch_add(1, std::memory_order_relaxed);
	return h;
}

Pin<> Handles::get(HANDLE h, HandleMeta *metaOut) {
	if (h == NO_HANDLE || isPseudo(h)) {
		return {}; // pseudo-handles have no entries
	}

	std::shared_lock lk(m);
	const auto idx = indexOf(h);
	if (idx >= mSlots.size()) {
		return {};
	}

	const auto &e = mSlots[idx];
	if (!e.obj) {
		return {};
	}
	if (metaOut) {
		*metaOut = e.meta;
	}
	return Pin<>::acquire(e.obj);
}

bool Handles::release(HANDLE h) {
	if (isPseudo(h)) {
		return true; // no-op, success
	}

	std::unique_lock lk(m);
	const auto idx = indexOf(h);
	if (idx >= mSlots.size()) {
		return false;
	}
	auto &e = mSlots[idx];
	if (!e.obj || e.meta.flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) {
		return false;
	}

	ObjectBase *obj = e.obj;
	const auto generation = e.meta.generation + 1;
	e = {}; // Clear entry
	e.meta.generation = generation;
	uint32_t handleCount = obj->handleCount.fetch_sub(1, std::memory_order_relaxed) - 1;

	if (idx <= kCompatMaxIndex) {
		mQuarantine.push_back(idx);
		if (mQuarantine.size() > kQuarantineLen) {
			mFreeBelow.push_back(mQuarantine.front());
			mQuarantine.pop_front();
		}
	} else {
		mFreeAbove.push_back(idx);
	}
	lk.unlock();

	if (handleCount == 0) {
		obj->onLastHandleClosed();
		if (mOnHandleZero)
			mOnHandleZero(obj);
	}
	detail::deref(obj);
	return true;
}

bool Handles::setInformation(HANDLE h, uint32_t mask, uint32_t value) {
	if (isPseudo(h)) {
		return true; // no-op, success
	}

	std::unique_lock lk(m);
	const auto idx = indexOf(h);
	if (idx >= mSlots.size()) {
		return false;
	}
	auto &e = mSlots[idx];
	if (!e.obj) {
		return false;
	}

	constexpr uint32_t kAllowedFlags = HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE;
	mask &= kAllowedFlags;

	e.meta.flags = (e.meta.flags & ~mask) | (value & mask);
	return true;
}

bool Handles::getInformation(HANDLE h, uint32_t *outFlags) const {
	if (!outFlags) {
		return false;
	}
	if (isPseudo(h)) {
		*outFlags = 0;
		return true;
	}
	std::shared_lock lk(m);
	const auto idx = indexOf(h);
	if (idx >= mSlots.size()) {
		return false;
	}
	const auto &e = mSlots[idx];
	if (!e.obj) {
		return false;
	}
	*outFlags = e.meta.flags;
	return true;
}

bool Handles::duplicateTo(HANDLE src, Handles &dst, HANDLE &out, uint32_t desiredAccess, bool inherit,
						  uint32_t options) {
	HandleMeta meta{};
	Pin<> obj = get(src, &meta);
	if (!obj) {
		return false;
	}

	bool closeSource = (options & DUPLICATE_CLOSE_SOURCE) != 0;
	if (closeSource && (meta.flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) != 0) {
		// Cannot close source if it is protected
		return false;
	}

	uint32_t effAccess = (options & DUPLICATE_SAME_ACCESS) ? meta.grantedAccess : (desiredAccess & meta.grantedAccess);
	const uint32_t flags = (inherit ? HANDLE_FLAG_INHERIT : 0);

	// Reuse the same handle if duplicating with DUPLICATE_CLOSE_SOURCE within the same table and no changes
	if (&dst == this && closeSource && effAccess == meta.grantedAccess && flags == meta.flags) {
		out = src;
		return true;
	}

	out = dst.alloc(std::move(obj), effAccess, flags);

	if (closeSource) {
		release(src);
	}
	return true;
}

DWORD Handles::snapshotInherited(std::optional<std::span<const HANDLE>> selection,
								 std::vector<HandleTransferEntry> &out) const {
	std::vector<HandleTransferEntry> snapshot;
	std::shared_lock lock(m);
	if (selection) {
		if (selection->size() > MAX_HANDLES)
			return ERROR_NOT_SUPPORTED;
		std::vector<uint32_t> indices;
		indices.reserve(selection->size());
		for (HANDLE handle : *selection) {
			if (handle <= 0 || static_cast<uint64_t>(handle) > UINT32_MAX)
				return ERROR_INVALID_HANDLE;
			const uint32_t index = indexOf(handle);
			if (index >= mSlots.size() || !mSlots[index].obj)
				return ERROR_INVALID_HANDLE;
			if (index >= MAX_HANDLES)
				return ERROR_NOT_SUPPORTED;
			if (!(mSlots[index].meta.flags & HANDLE_FLAG_INHERIT))
				return ERROR_INVALID_PARAMETER;
			indices.push_back(index);
		}
		std::sort(indices.begin(), indices.end());
		indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
		snapshot.reserve(indices.size());
		for (uint32_t index : indices) {
			const auto &entry = mSlots[index];
			snapshot.push_back(
				{makeHandle(index), Pin<>::acquire(entry.obj), entry.meta.grantedAccess, entry.meta.flags});
		}
	} else {
		for (uint32_t index = 0; index < mSlots.size(); ++index) {
			const auto &entry = mSlots[index];
			if (!entry.obj || !(entry.meta.flags & HANDLE_FLAG_INHERIT))
				continue;
			if (index >= MAX_HANDLES || snapshot.size() == MAX_HANDLES)
				return ERROR_NOT_SUPPORTED;
			snapshot.push_back(
				{makeHandle(index), Pin<>::acquire(entry.obj), entry.meta.grantedAccess, entry.meta.flags});
		}
	}
	lock.unlock();
	out = std::move(snapshot);
	return ERROR_SUCCESS;
}

DWORD Handles::snapshotSelected(std::span<const HANDLE> selection, std::vector<HandleTransferEntry> &out) const {
	if (selection.size() > MAX_HANDLES)
		return ERROR_NOT_SUPPORTED;
	std::vector<HandleTransferEntry> snapshot;
	std::shared_lock lock(m);
	for (HANDLE handle : selection) {
		if (handle == NO_HANDLE || handle == static_cast<HANDLE>(-1))
			continue;
		if (handle <= 0 || static_cast<uint64_t>(handle) > UINT32_MAX || (static_cast<uint32_t>(handle) & 3))
			return ERROR_INVALID_HANDLE;
		const uint32_t index = indexOf(handle);
		if (index >= mSlots.size() || !mSlots[index].obj)
			return ERROR_INVALID_HANDLE;
		const auto &entry = mSlots[index];
		snapshot.push_back({handle, Pin<>::acquire(entry.obj), entry.meta.grantedAccess, entry.meta.flags});
	}
	lock.unlock();
	out = std::move(snapshot);
	return ERROR_SUCCESS;
}

DWORD Handles::importExact(std::span<HandleTransferEntry> entries) {
	std::unique_lock lock(m);
	if (!mSlots.empty())
		return ERROR_INVALID_DATA;
	if (entries.size() > MAX_HANDLES)
		return ERROR_NOT_SUPPORTED;
	constexpr uint32_t allowedFlags = HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE;
	std::vector<bool> occupied(MAX_HANDLES, false);
	uint32_t slotCount = 0;
	for (const auto &entry : entries) {
		if (!entry.object || entry.handle <= 0 || static_cast<uint64_t>(entry.handle) > UINT32_MAX ||
			(static_cast<uint32_t>(entry.handle) & 3) || (entry.flags & ~allowedFlags))
			return ERROR_INVALID_DATA;
		const uint32_t index = indexOf(entry.handle);
		if (index >= MAX_HANDLES)
			return ERROR_NOT_SUPPORTED;
		if (occupied[index])
			return ERROR_INVALID_DATA;
		occupied[index] = true;
		slotCount = std::max(slotCount, index + 1);
	}
	std::vector<Entry> slots(slotCount);
	std::vector<uint32_t> freeBelow, freeAbove;
	for (uint32_t index = 0; index < slotCount; ++index) {
		if (!occupied[index])
			(index <= kCompatMaxIndex ? freeBelow : freeAbove).push_back(index);
	}
	// All validation and allocation precede publication and reference transfer.
	for (auto &entry : entries) {
		auto &slot = slots[indexOf(entry.handle)];
		slot.obj = entry.object.release();
		slot.meta = {entry.grantedAccess, entry.flags, slot.obj->type, 1};
		slot.obj->handleCount.fetch_add(1, std::memory_order_relaxed);
	}
	mSlots = std::move(slots);
	mFreeBelow = std::move(freeBelow);
	mFreeAbove = std::move(freeAbove);
	nextIndex = slotCount;
	return ERROR_SUCCESS;
}

bool Namespace::insert(const std::u16string &name, ObjectBase *obj, bool permanent) {
	if (name.empty() || !obj) {
		return false;
	}
	std::unique_lock lk(m);
	// Namespace holds a weak ref
	const auto [_, inserted] = mTable.try_emplace(name, obj, permanent);
	return inserted;
}

void Namespace::remove(ObjectBase *obj) {
	std::unique_lock lk(m);
	for (auto it = mTable.begin(); it != mTable.end(); ++it) {
		if (it->second.obj == obj && !it->second.permanent) {
			mTable.erase(it);
			break;
		}
	}
}

Pin<> Namespace::get(const std::u16string &name) {
	if (name.empty()) {
		return {};
	}
	std::shared_lock lk(m);
	auto it = mTable.find(name);
	if (it == mTable.end()) {
		return {};
	}
	assert(it->second.obj);
	return Pin<>::acquire(it->second.obj);
}

void WaitableObject::registerWaiter(void *context, DWORD index, WaiterCallback cb) {
	if (!cb) {
		return;
	}
	std::lock_guard lk(waitersMutex);
	waiters.push_back(std::make_shared<Waiter>(cb, context, index));
}

void WaitableObject::unregisterWaiter(void *context) {
	std::vector<std::shared_ptr<Waiter>> removed;
	{
		std::lock_guard lk(waitersMutex);
		auto firstRemoved = std::remove_if(waiters.begin(), waiters.end(), [&](const auto &waiter) {
			if (waiter->context != context) {
				return false;
			}
			removed.push_back(waiter);
			return true;
		});
		waiters.erase(firstRemoved, waiters.end());
	}
	for (const auto &waiter : removed) {
		std::unique_lock lk(waiter->mutex);
		waiter->active = false;
		waiter->cv.wait(lk, [&] { return waiter->callbacksInFlight == 0; });
	}
}

void WaitableObject::notifyWaiters(bool abandoned) {
	// WaitBlock callbacks only update their owning wait state; they never
	// register or unregister waiters. Holding the registration lock across the
	// callback therefore gives unregisterWaiter a strict lifetime barrier and
	// avoids allocating/copying a shared_ptr vector in ReleaseSemaphore's hot
	// path (and during allocator/bootstrap notifications).
	std::lock_guard registrationsLock(waitersMutex);
	for (const auto &waiter : waiters) {
		{
			std::lock_guard lk(waiter->mutex);
			if (!waiter->active || !waiter->callback) {
				continue;
			}
			++waiter->callbacksInFlight;
		}
		waiter->callback(waiter->context, this, waiter->index, abandoned);
		{
			std::lock_guard lk(waiter->mutex);
			--waiter->callbacksInFlight;
			if (!waiter->active && waiter->callbacksInFlight == 0) {
				waiter->cv.notify_all();
			}
		}
	}
}

namespace wibo {

Namespace g_namespace;
Handles &handles() {
	static Handles table([](ObjectBase *obj) { g_namespace.remove(obj); });
	return table;
}

} // namespace wibo
