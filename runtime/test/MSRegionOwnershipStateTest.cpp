#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define MRT_USE_MARK_SWEEP 1
#define MRT_BASE_LOG_H

#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define DCHECK(...)
#define LOG(...)
#define DLOG(...)
#define RTLOG_FATAL 0
#define RTLOG_INFO 1
#define RTLOG_ERROR 2
#define CHECK_DETAIL(x, ...)                                                                     \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "CHECK_DETAIL failed: %s:%d: %s\n", __FILE__, __LINE__, #x);   \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#define MRT_ASSERT(x, msg)                                                                       \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "MRT_ASSERT failed: %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)

namespace MapleRuntime {
const size_t MRT_PAGE_SIZE = 16 * 1024;
}

#include "src/Heap/MAllocator/MSBitmapArena.h"
#include "src/Heap/MAllocator/MSSizeClass.h"

namespace MapleRuntime {
uint32_t SizeClassInfo::slotDivMagic_[kNumSizeClasses] = {};

uint64_t* MSBitmapArena::GetSlotBase(size_t)
{
    return nullptr;
}
} // namespace MapleRuntime

#include "src/Heap/MAllocator/MSRegionInfo.cpp"

namespace {

using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSRegionInfoArray;
using MapleRuntime::MSRegionLocation;
using MapleRuntime::MS_REGION_SIZE;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectEqLocation(MSRegionLocation actual, MSRegionLocation expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
        std::abort();
    }
}

void ExpectEqOwner(uintptr_t actual, uintptr_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%p actual=%p\n",
                     message,
                     reinterpret_cast<void*>(expected),
                     reinterpret_cast<void*>(actual));
        std::abort();
    }
}

void TestTrackedRegionStateTransitions()
{
    void* heap = std::malloc(MS_REGION_SIZE * 2);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    std::vector<MSRegionInfo> infos(2);
    MSRegionInfoArray::Init(reinterpret_cast<uintptr_t>(heap), infos.size(), infos.data());

    uintptr_t owner = 0x12345678ULL;
    MSRegionInfoArray::TransitionState(&infos[0],
                                       MSRegionLocation::FREE,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       "test free -> transient");
    MSRegionInfoArray::TransitionState(&infos[0],
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionLocation::ACTIVE_LOCAL,
                                       MSRegionInfoArray::ANY_OWNER,
                                       owner,
                                       "test transient -> active");

    ExpectEqLocation(MSRegionInfoArray::GetLocation(&infos[0]),
                     MSRegionLocation::ACTIVE_LOCAL,
                     "tracked region should publish active location");
    ExpectEqOwner(MSRegionInfoArray::GetOwnerToken(&infos[0]),
                  owner,
                  "tracked region should publish owner token");

    MSRegionInfoArray::TransitionState(&infos[0],
                                       MSRegionLocation::ACTIVE_LOCAL,
                                       MSRegionLocation::TRANSIENT,
                                       owner,
                                       0,
                                       "test active -> transient");
    ExpectEqLocation(MSRegionInfoArray::GetLocation(&infos[0]),
                     MSRegionLocation::TRANSIENT,
                     "tracked region should return to transient");
    ExpectEqOwner(MSRegionInfoArray::GetOwnerToken(&infos[0]),
                  0,
                  "transient state should clear owner token");

    MSRegionInfoArray::Cleanup();
    std::free(heap);
}

void TestUntrackedRegionStateApisNoop()
{
    MSRegionInfo stackRegion {};
    ExpectEqLocation(MSRegionInfoArray::GetLocation(&stackRegion),
                     MSRegionLocation::UNKNOWN,
                     "stack region should stay untracked");
    ExpectEqOwner(MSRegionInfoArray::GetOwnerToken(&stackRegion),
                  0,
                  "stack region should not have owner");

    MSRegionInfoArray::SetState(&stackRegion, MSRegionLocation::ACTIVE_LOCAL, 0xABC);

    ExpectEqLocation(MSRegionInfoArray::GetLocation(&stackRegion),
                     MSRegionLocation::UNKNOWN,
                     "SetState should ignore untracked regions");
    ExpectEqOwner(MSRegionInfoArray::GetOwnerToken(&stackRegion),
                  0,
                  "SetState should not attach owner to untracked regions");
}

} // namespace

int main()
{
    TestTrackedRegionStateTransitions();
    TestUntrackedRegionStateApisNoop();
    return 0;
}
