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

void TestResetStartsNewRootSet()
{
    MSRootDedupTable table;
    table.Reset(64);
    Expect(table.Insert(static_cast<uintptr_t>(0x3000)), "first-cycle root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x4000)), "second first-cycle root must be accepted");

    table.Reset(1);
    Expect(table.Insert(static_cast<uintptr_t>(0x3000)), "reset must start a new root set");
}

void TestResetReacceptsPriorRoots()
{
    MSRootDedupTable table;
    table.Reset(1024);
    Expect(table.Insert(static_cast<uintptr_t>(0x5000)), "first sparse root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x6000)), "second sparse root must be accepted");

    table.Reset(1024);
    Expect(table.Insert(static_cast<uintptr_t>(0x5000)),
           "a selectively cleared address must be accepted in the next epoch");
}

void TestHandlesHashCollisions()
{
    MSRootDedupTable table;
    table.Reset(3);
    // Reset(3) uses an eight-slot table. Root addresses differ by eight
    // bitmap-aligned words, so their multiplicative hashes share one slot.
    Expect(table.Insert(static_cast<uintptr_t>(0x4000)), "first colliding root must be accepted");
    Expect(table.Insert(static_cast<uintptr_t>(0x4040)),
           "different colliding root must be accepted");
    Expect(!table.Insert(static_cast<uintptr_t>(0x4040)),
           "colliding duplicate root must be rejected");
}

} // namespace

int main()
{
    TestRejectsDuplicateRoots();
    TestResetStartsNewRootSet();
    TestResetReacceptsPriorRoots();
    TestHandlesHashCollisions();
    return 0;
}
