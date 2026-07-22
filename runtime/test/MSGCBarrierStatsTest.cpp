#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MBarrier/MSGCBarrierStats.cpp"

namespace {

using MapleRuntime::GetMSGCBarrierStatsForTesting;
using MapleRuntime::MSGCBarrierKind;
using MapleRuntime::RecordMSGCBarrierRuntimeCall;
using MapleRuntime::RecordMSGCMarkBarrierCall;
using MapleRuntime::ResetMSGCBarrierStatsForTesting;
using MapleRuntime::SetMSGCBarrierStatsEnabledForTesting;

void ExpectEq(unsigned long long actual, unsigned long long expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%llu actual=%llu\n", message, expected, actual);
        std::abort();
    }
}

void TestRuntimeAndMarkBarrierCounters()
{
    SetMSGCBarrierStatsEnabledForTesting(true);
    ResetMSGCBarrierStatsForTesting();

    RecordMSGCBarrierRuntimeCall(MSGCBarrierKind::WRITE_REF, false);
    RecordMSGCBarrierRuntimeCall(MSGCBarrierKind::WRITE_REF, true);
    RecordMSGCBarrierRuntimeCall(MSGCBarrierKind::ARRAY_COPY_STRUCT, true);
    RecordMSGCMarkBarrierCall(MSGCBarrierKind::WRITE_REF);
    RecordMSGCMarkBarrierCall(MSGCBarrierKind::ARRAY_COPY_STRUCT);
    RecordMSGCMarkBarrierCall(MSGCBarrierKind::ARRAY_COPY_STRUCT);

    MapleRuntime::MSGCBarrierStatsSnapshot snapshot = GetMSGCBarrierStatsForTesting();
    constexpr size_t kWriteRef = static_cast<size_t>(MSGCBarrierKind::WRITE_REF);
    constexpr size_t kArrayCopyStruct = static_cast<size_t>(MSGCBarrierKind::ARRAY_COPY_STRUCT);

    ExpectEq(snapshot.runtimeTotal[kWriteRef], 2, "write_ref runtime total");
    ExpectEq(snapshot.runtimeBarrier[kWriteRef], 1, "write_ref runtime barrier");
    ExpectEq(snapshot.markBarrier[kWriteRef], 1, "write_ref mark barrier");
    ExpectEq(snapshot.runtimeTotal[kArrayCopyStruct], 1, "array_copy_struct runtime total");
    ExpectEq(snapshot.runtimeBarrier[kArrayCopyStruct], 1, "array_copy_struct runtime barrier");
    ExpectEq(snapshot.markBarrier[kArrayCopyStruct], 2, "array_copy_struct mark barrier");
}

} // namespace

int main()
{
    TestRuntimeAndMarkBarrierCounters();
    return 0;
}
