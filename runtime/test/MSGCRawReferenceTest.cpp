#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/Barrier/GCRefField.inline.h"

// RefField's debug-only CHECK paths reference the runtime logger even in a
// standalone release-mode test. These stubs keep this unit test independent
// from the complete runtime link; a reached CHECK remains fatal.
namespace MapleRuntime {
Logger& Logger::GetLogger() noexcept
{
    std::abort();
}

void Logger::FormatLog(RTLogLevel, bool, const char*, ...) noexcept
{
    std::abort();
}
} // namespace MapleRuntime

namespace {

using MapleRuntime::BaseObject;
using MapleRuntime::LoadReferenceForGCMode;
using MapleRuntime::MSCompareExchangeRawReference;
using MapleRuntime::MSExchangeRawReference;
using MapleRuntime::MSLoadRawReference;
using MapleRuntime::MSStoreRawReference;
using MapleRuntime::RefField;
using MapleRuntime::StoreReferenceForGCMode;

void ExpectBits(BaseObject* actual, uintptr_t expected, const char* message)
{
    uintptr_t actualBits = reinterpret_cast<uintptr_t>(actual);
    if (actualBits != expected) {
        std::fprintf(stderr, "%s: expected=0x%zx actual=0x%zx\n", message, expected, actualBits);
        std::abort();
    }
}

void ExpectValue(const RefField<false>& field, uintptr_t expected, const char* message)
{
    uintptr_t actual = static_cast<uintptr_t>(field.GetFieldValue());
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=0x%zx actual=0x%zx\n", message, expected, actual);
        std::abort();
    }
}

void TestNonAtomicRawReferencePreservesAllBits()
{
    constexpr uintptr_t kValue = UINT64_C(0xa5a5123456789abc);
    RefField<false> field(static_cast<MapleRuntime::MAddress>(0));
    BaseObject* ref = reinterpret_cast<BaseObject*>(kValue);

    MSStoreRawReference(field, ref);

    ExpectValue(field, kValue, "non-atomic raw store");
    ExpectBits(MSLoadRawReference(field), kValue, "non-atomic raw load");
}

void TestAtomicRawReferencePreservesAllBits()
{
    constexpr uintptr_t kFirst = UINT64_C(0xb6b623456789abcd);
    constexpr uintptr_t kSecond = UINT64_C(0xc7c73456789abcde);
    constexpr uintptr_t kThird = UINT64_C(0xd8d8456789abcdef);
    RefField<true> field(static_cast<MapleRuntime::MAddress>(0));

    BaseObject* first = reinterpret_cast<BaseObject*>(kFirst);
    BaseObject* second = reinterpret_cast<BaseObject*>(kSecond);
    BaseObject* third = reinterpret_cast<BaseObject*>(kThird);

    MSStoreRawReference(field, first, std::memory_order_release);
    ExpectBits(MSLoadRawReference(field, std::memory_order_acquire), kFirst,
               "atomic raw load/store");

    ExpectBits(MSExchangeRawReference(field, second, std::memory_order_acq_rel), kFirst,
               "atomic raw exchange old value");
    ExpectBits(MSLoadRawReference(field), kSecond, "atomic raw exchange new value");

    if (!MSCompareExchangeRawReference(field, second, third,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        std::fprintf(stderr, "atomic raw compare-exchange unexpectedly failed\n");
        std::abort();
    }
    ExpectBits(MSLoadRawReference(field), kThird, "atomic raw compare-exchange new value");
}

void TestCommonGCModeHelperSelectsRawMSGCRepresentation()
{
    constexpr uintptr_t kValue = UINT64_C(0xe9e956789abcdef0);
    RefField<false> field(static_cast<MapleRuntime::MAddress>(0));
    BaseObject* ref = reinterpret_cast<BaseObject*>(kValue);

    StoreReferenceForGCMode(field, ref);

    ExpectValue(field, kValue, "GC-mode MSGC store");
    ExpectBits(LoadReferenceForGCMode(field), kValue, "GC-mode MSGC load");
}

} // namespace

int main()
{
    TestNonAtomicRawReferencePreservesAllBits();
    TestAtomicRawReferencePreservesAllBits();
    TestCommonGCModeHelperSelectsRawMSGCRepresentation();
    return 0;
}
