#include "errors.h"
#include "handles.h"
#include "test_assert.h"

#include <array>
#include <span>
#include <vector>

namespace {

struct TransferObject final : ObjectBase {
	int &destroyed;
	unsigned value = 7;
	explicit TransferObject(int &destroyed) : ObjectBase(ObjectType::File), destroyed(destroyed) {}
	~TransferObject() override { ++destroyed; }
};

void snapshotAndImport() {
	int destroyed = 0, excludedDestroyed = 0, freshDestroyed = 0;
	Handles source(nullptr), destination(nullptr);
	auto object = make_pin<TransferObject>(destroyed);
	auto *identity = object.get();
	const HANDLE first = source.alloc(object.clone(), 1, HANDLE_FLAG_INHERIT);
	const HANDLE excluded = source.alloc(make_pin<TransferObject>(excludedDestroyed), 3, 0);
	const HANDLE alias = source.alloc(std::move(object), 2, HANDLE_FLAG_INHERIT);
	TEST_CHECK_EQ(4, first);
	TEST_CHECK_EQ(8, excluded);
	TEST_CHECK_EQ(12, alias);
	std::vector<HandleTransferEntry> entries;
	TEST_CHECK_EQ(ERROR_SUCCESS, source.snapshotInherited(std::nullopt, entries));
	TEST_CHECK_EQ(2, entries.size());
	TEST_CHECK_EQ(first, entries[0].handle);
	TEST_CHECK_EQ(alias, entries[1].handle);
	TEST_CHECK(entries[0].object.get() == identity && entries[1].object.get() == identity);
	TEST_CHECK_EQ(1, entries[0].grantedAccess);
	TEST_CHECK_EQ(2, entries[1].grantedAccess);
	TEST_CHECK(source.release(first));
	TEST_CHECK(source.release(alias));
	TEST_CHECK_EQ(0, destroyed);
	TEST_CHECK_EQ(ERROR_SUCCESS, destination.importExact(entries));
	TEST_CHECK(!entries[0].object && !entries[1].object);
	TEST_CHECK_EQ(2, identity->handleCount.load());
	{
		HandleMeta metadata{};
		auto firstObject = destination.get(first, &metadata);
		TEST_CHECK(firstObject.get() == identity);
		TEST_CHECK_EQ(1, metadata.grantedAccess);
		TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, metadata.flags);
		auto aliasObject = destination.get(alias, &metadata);
		TEST_CHECK(aliasObject.get() == identity);
		TEST_CHECK_EQ(2, metadata.grantedAccess);
		static_cast<TransferObject *>(firstObject.get())->value = 11;
		TEST_CHECK_EQ(11, static_cast<TransferObject *>(aliasObject.get())->value);
	}
	TEST_CHECK(!destination.get(excluded));
	const HANDLE fresh = destination.alloc(make_pin<TransferObject>(freshDestroyed), 3, 0);
	TEST_CHECK_EQ(16, fresh);
	TEST_CHECK(destination.get(first).get() == identity && destination.get(alias).get() == identity);
	std::array<HANDLE, 3> selection{alias, first, alias};
	TEST_CHECK_EQ(ERROR_SUCCESS, destination.snapshotInherited(std::span<const HANDLE>(selection), entries));
	TEST_CHECK_EQ(2, entries.size());
	std::array<HANDLE, 1> invalidSelection{fresh};
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER,
				  destination.snapshotInherited(std::span<const HANDLE>(invalidSelection), entries));
	TEST_CHECK_EQ(2, entries.size());
	TEST_CHECK(entries[0].object.get() == identity && entries[1].object.get() == identity);
	invalidSelection[0] = 20;
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE,
				  destination.snapshotInherited(std::span<const HANDLE>(invalidSelection), entries));
	TEST_CHECK_EQ(2, entries.size());
	TEST_CHECK(destination.release(first));
	TEST_CHECK(destination.release(alias));
	TEST_CHECK_EQ(0, identity->handleCount.load());
	TEST_CHECK_EQ(0, destroyed);
	TEST_CHECK_EQ(ERROR_SUCCESS, destination.snapshotInherited(std::span<const HANDLE>{}, entries));
	TEST_CHECK(entries.empty());
	TEST_CHECK_EQ(1, destroyed);
	TEST_CHECK(destination.release(fresh));
	TEST_CHECK_EQ(1, freshDestroyed);
	TEST_CHECK(source.release(excluded));
	TEST_CHECK_EQ(1, excludedDestroyed);
}

void atomicImportFailure() {
	int destroyed = 0;
	Handles table(nullptr);
	auto object = make_pin<TransferObject>(destroyed);
	std::array<HandleTransferEntry, 2> entries{{
		{4, object.clone(), 1, HANDLE_FLAG_INHERIT},
		{4, object.clone(), 2, 0},
	}};
	const auto expectUnchanged = [&](DWORD expected) {
		TEST_CHECK_EQ(expected, table.importExact(entries));
		TEST_CHECK(entries[0].object.get() == object.get() && entries[1].object.get() == object.get());
		TEST_CHECK_EQ(0, object->handleCount.load());
		TEST_CHECK(!table.get(4));
		TEST_CHECK_EQ(0, destroyed);
	};
	expectUnchanged(ERROR_INVALID_DATA);
	entries[1].handle = 6;
	expectUnchanged(ERROR_INVALID_DATA);
	entries[1].handle = static_cast<HANDLE>((MAX_HANDLES + 1) << 2);
	expectUnchanged(ERROR_NOT_SUPPORTED);
	entries[1].handle = 12;
	entries[1].flags = 4;
	expectUnchanged(ERROR_INVALID_DATA);
	entries[1].flags = 0;
#ifdef WIBO_GUEST_64
	entries[1].handle = 0x100000004LL;
	expectUnchanged(ERROR_INVALID_DATA);
	entries[1].handle = 12;
#endif
	TEST_CHECK_EQ(ERROR_SUCCESS, table.importExact(entries));
	TEST_CHECK(!entries[0].object && !entries[1].object);
	TEST_CHECK_EQ(2, object->handleCount.load());
	std::array<HandleTransferEntry, 1> extra{{{8, object.clone(), 3, 0}}};
	TEST_CHECK_EQ(ERROR_INVALID_DATA, table.importExact(extra));
	TEST_CHECK(extra[0].object.get() == object.get());
	TEST_CHECK(!table.get(8));
	TEST_CHECK_EQ(2, object->handleCount.load());
	table.clear();
	TEST_CHECK_EQ(0, object->handleCount.load());
	TEST_CHECK_EQ(0, destroyed);
}

} // namespace

int main() {
	snapshotAndImport();
	atomicImportFailure();
	return 0;
}
