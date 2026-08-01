#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSRootDedupTable.h"

namespace {

using MapleRuntime::MSRootDedupTable;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestRejectsDuplicateRoots()
{
    MSRootDedupTable table;
    table.Reset(4);
    Expect(table.Insert(static_cast<uintptr_t>(0x1000)), "first root must be accepted");
    Expect(!table.Insert(static_cast<uintptr_t>(0x1000)), "duplicate root must be rejected");
    Expect(table.Insert(static_cast<uintptr_t>(0x2000)), "different root must be accepted");
}

void TestResetStartsNewRootSetAndReusesCapacity()
{
    MSRootDedupTable table;
    table.Reset(64);
    size_t reservedCapacity = table.Capacity();
    Expect(reservedCapacity >= 128,
           "dedup table must keep load factor at or below one half");
    Expect(table.Insert(static_cast<uintptr_t>(0x3000)), "first-cycle root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x4000)), "second first-cycle root must be accepted");
    Expect(table.UsedSlotCount() == 2, "dedup table must track only occupied slots");

    table.Reset(1);
    Expect(table.LastResetClearedSlotCount() == 2,
           "reset must clear only slots occupied by the preceding cycle");
    Expect(table.UsedSlotCount() == 0, "reset must empty the used-slot list");
    Expect(table.Insert(static_cast<uintptr_t>(0x3000)), "reset must start a new root set");
    Expect(table.Capacity() >= 2, "single root still needs an empty probe slot");
    Expect(table.ReservedCapacity() >= reservedCapacity,
           "reset must reuse the previously allocated backing storage");
}

void TestResetDoesNotClearTheWholeRetainedTable()
{
    MSRootDedupTable table;
    table.Reset(1024);
    const size_t tableCapacity = table.Capacity();
    Expect(tableCapacity >= 2048, "large root set must reserve a low-load table");
    Expect(table.Insert(static_cast<uintptr_t>(0x5000)), "first sparse root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x6000)), "second sparse root must be accepted");

    table.Reset(1024);
    Expect(table.LastResetClearedSlotCount() == 2,
           "sparse reset must not clear every retained table slot");
    Expect(table.LastResetClearedSlotCount() < tableCapacity,
           "used-slot reset must perform less work than a full clear");
    Expect(table.Insert(static_cast<uintptr_t>(0x5000)),
           "a selectively cleared address must be accepted in the next epoch");
}

void TestHandlesHashCollisions()
{
    MSRootDedupTable table;
    table.Reset(3);
    uintptr_t stride = static_cast<uintptr_t>(table.Capacity()) << 3;
    Expect(table.Insert(static_cast<uintptr_t>(0x4000)), "first colliding root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x4000) + stride),
           "different colliding root must be accepted");
    Expect(!table.Insert(static_cast<uintptr_t>(0x4000) + stride),
           "colliding duplicate root must be rejected");
}

} // namespace

int main()
{
    TestRejectsDuplicateRoots();
    TestResetStartsNewRootSetAndReusesCapacity();
    TestResetDoesNotClearTheWholeRetainedTable();
    TestHandlesHashCollisions();
    return 0;
}
