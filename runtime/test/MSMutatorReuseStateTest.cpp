#define MRT_USE_MARK_SWEEP 1

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "src/Mutator/ThreadLocal.h"
#include "src/Mutator/SatbBuffer.h"
#include "src/Mutator/Mutator.h"

namespace MapleRuntime {

SatbBuffer& SatbBuffer::Instance() noexcept
{
    static SatbBuffer buffer;
    return buffer;
}

void FrameInfo::PrintFrameInfo(uint32_t) const {}

} // namespace MapleRuntime

namespace {

using MapleRuntime::GCPhase;
using MapleRuntime::Mutator;
using MapleRuntime::ThreadLocal;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s: expected true\n", message);
        std::abort();
    }
}

void ExpectFalse(bool value, const char* message)
{
    if (value) {
        std::fprintf(stderr, "%s: expected false\n", message);
        std::abort();
    }
}

void ExpectEqU32(uint32_t actual, uint32_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expected, actual);
        std::abort();
    }
}

void ExpectEqU8(uint8_t actual, uint8_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expected, actual);
        std::abort();
    }
}

void ExpectEqPhase(GCPhase actual, GCPhase expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
        std::abort();
    }
}

void ExpectEqTransition(Mutator::GCPhaseTransitionState actual,
                        Mutator::GCPhaseTransitionState expected,
                        const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
        std::abort();
    }
}

void ExpectEqPtr(const void* actual, const void* expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%p actual=%p\n", message, expected, actual);
        std::abort();
    }
}

void TestRetireForCJThreadReuseResetsPhaseHandshakeState()
{
    Mutator mutator;
    mutator.Init();
    mutator.SetInSaferegion(Mutator::SAFE_REGION_FALSE);
    mutator.SetSuspensionFlag(Mutator::SUSPENSION_FOR_GC_PHASE);
    mutator.PrepareEnumAssist(17);

    ExpectEqTransition(mutator.GetTransitionStateForDebug(),
                       Mutator::NEED_TRANSITION,
                       "precondition: gc-phase request should arm transition");
    ExpectTrue(mutator.IsEnumAssistPending(17),
               "precondition: enum-assist epoch should be pending");

    mutator.RetireForCJThreadReuse(GCPhase::GC_PHASE_MS_ENUM);

    ExpectTrue(mutator.InSaferegion(), "retired mutator should stay in saferegion");
    ExpectEqU32(mutator.GetSuspensionFlag(), 0, "retired mutator should clear suspension flags");
    ExpectEqTransition(mutator.GetTransitionStateForDebug(),
                       Mutator::FINISH_TRANSITION,
                       "retired mutator should look phase-complete to GC");
    ExpectEqPhase(mutator.GetMutatorPhase(),
                  GCPhase::GC_PHASE_MS_ENUM,
                  "retired mutator should publish current global phase");
    ExpectFalse(mutator.IsEnumAssistPending(17),
                "retired mutator should drop stale enum-assist debt");
}

void TestPrepareForCJThreadReuseRebasesDormantMutatorToCurrentPhase()
{
    Mutator mutator;
    mutator.Init();
    mutator.RetireForCJThreadReuse(GCPhase::GC_PHASE_MS_ENUM);
    mutator.PrepareForCJThreadReuse(GCPhase::GC_PHASE_IDLE);

    ExpectTrue(mutator.InSaferegion(), "prepared dormant mutator remains parked until PreparedToRun");
    ExpectEqU32(mutator.GetSuspensionFlag(), 0, "prepared dormant mutator should stay flag-clean");
    ExpectEqTransition(mutator.GetTransitionStateForDebug(),
                       Mutator::FINISH_TRANSITION,
                       "prepared dormant mutator should stay phase-complete");
    ExpectEqPhase(mutator.GetMutatorPhase(),
                  GCPhase::GC_PHASE_IDLE,
                  "prepared dormant mutator should rebase to current phase");
}

void TestThreadLocalEncodedGCPhaseRoundTrips()
{
    ThreadLocal::GetThreadLocalData()->encodedGCPhase = static_cast<uint8_t>(GCPhase::GC_PHASE_MS_ENUM);
    ExpectEqU8(ThreadLocal::GetThreadLocalData()->encodedGCPhase,
               static_cast<uint8_t>(GCPhase::GC_PHASE_MS_ENUM),
               "encoded gc phase should remain the published TLS byte");
}

void TestRebaseManagedContextFromC2NStubPublishesLiveStubFrame()
{
    Mutator mutator;
    mutator.Init();

    MapleRuntime::FrameAddress c2nStub {
        reinterpret_cast<MapleRuntime::FrameAddress*>(0x12345000),
        reinterpret_cast<const uint32_t*>(0x10203040)
    };
    auto* publishedPc = reinterpret_cast<const uint32_t*>(0xABCDEF00);

    auto& uwContext = mutator.GetUnwindContext();
    uwContext.anchorFA = reinterpret_cast<uint32_t*>(0x77889900);
    uwContext.frameInfo.mFrame.SetFA(reinterpret_cast<MapleRuntime::FrameAddress*>(0xCAFEBABE));
    uwContext.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(0xDEADBEEF));
    uwContext.SetUnwindContextStatus(MapleRuntime::UnwindContextStatus::RISKY);

    mutator.RebaseManagedContextFromC2NStub(publishedPc, &c2nStub);

    ExpectEqPtr(uwContext.frameInfo.mFrame.GetFA(),
                &c2nStub,
                "rebased unwind context should point at live stub frame");
    ExpectEqPtr(uwContext.frameInfo.mFrame.GetIP(),
                publishedPc,
                "rebased unwind context should publish stub unwind pc");
    ExpectEqU32(static_cast<uint32_t>(uwContext.GetUnwindContextStatus()),
                static_cast<uint32_t>(MapleRuntime::UnwindContextStatus::RELIABLE),
                "rebased unwind context should become reliable");
    ExpectTrue(mutator.IsManagedReturnWindowActiveForDebug(),
               "rebasing should mark managed-return window active");
    ExpectEqPtr(uwContext.anchorFA,
                reinterpret_cast<uint32_t*>(0x77889900),
                "rebasing should preserve anchor frame");

    mutator.FinishManagedReturnWindow();
    ExpectFalse(mutator.IsManagedReturnWindowActiveForDebug(),
                "managed-return window should be clearable after stub cleanup");
}

void TestEnumAssistTargetFilterRejectsUnstableRemoteWindows()
{
    Mutator mutator;
    mutator.Init();
    mutator.SetInSaferegion(Mutator::SAFE_REGION_TRUE);

    auto& uwContext = mutator.GetUnwindContext();
    uwContext.SetUnwindContextStatus(MapleRuntime::UnwindContextStatus::RELIABLE);
    ExpectTrue(mutator.CanServeAsEnumAssistTargetForDebug(),
               "stable saferegion mutator should be assist-claimable");

    uwContext.SetUnwindContextStatus(MapleRuntime::UnwindContextStatus::SIGNAL_STATUS);
    ExpectFalse(mutator.CanServeAsEnumAssistTargetForDebug(),
                "live safepoint-handler window must not be assist-claimable");

    uwContext.SetUnwindContextStatus(MapleRuntime::UnwindContextStatus::RELIABLE);
    mutator.RebaseManagedContextFromC2NStub(reinterpret_cast<const uint32_t*>(0xABC00000),
                                           reinterpret_cast<MapleRuntime::FrameAddress*>(0x12345000));
    ExpectFalse(mutator.CanServeAsEnumAssistTargetForDebug(),
                "managed return window must not be assist-claimable");

    mutator.FinishManagedReturnWindow();
    ExpectTrue(mutator.CanServeAsEnumAssistTargetForDebug(),
               "closing the return window should restore assist eligibility");
}

} // namespace

int main()
{
    TestRetireForCJThreadReuseResetsPhaseHandshakeState();
    TestPrepareForCJThreadReuseRebasesDormantMutatorToCurrentPhase();
    TestThreadLocalEncodedGCPhaseRoundTrips();
    TestRebaseManagedContextFromC2NStubPublishesLiveStubFrame();
    TestEnumAssistTargetFilterRejectsUnstableRemoteWindows();
    return 0;
}
