#include <cassert>
#include <cstddef>
#include <cstdint>

#include "src/Heap/MSGCFastPath.h"

namespace MapleRuntime {

extern "C" MSGCFastPathState CJ_MRT_MSGCFastPathState = {};

} // namespace MapleRuntime

int main() {
  using namespace MapleRuntime;

  static_assert(sizeof(MSGCFastPathState) == 32, "MSGC fast-path state size");
  static_assert(offsetof(MSGCFastPathState, barrierPhase) == 0,
                "barrier phase offset");
  static_assert(offsetof(MSGCFastPathState, heapReservedStart) == 8,
                "heap start offset");
  static_assert(offsetof(MSGCFastPathState, heapReservedEnd) == 16,
                "heap end offset");
  static_assert(offsetof(MSGCFastPathState, regionInfoArray) == 24,
                "region info offset");

  PublishMSGCFastPathHeapRange(0x100000000ULL, 0x200000000ULL);
  PublishMSGCFastPathRegionInfoArray(0x300000000ULL);
  PublishMSGCFastPathBarrierPhase(1);

  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.heapReservedStart,
                         __ATOMIC_ACQUIRE) == 0x100000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.heapReservedEnd,
                         __ATOMIC_ACQUIRE) == 0x200000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.regionInfoArray,
                         __ATOMIC_ACQUIRE) == 0x300000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.barrierPhase,
                         __ATOMIC_ACQUIRE) == 1);

  PublishMSGCFastPathRegionInfoArray(0);
  PublishMSGCFastPathBarrierPhase(0);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.regionInfoArray,
                         __ATOMIC_ACQUIRE) == 0);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.barrierPhase,
                         __ATOMIC_ACQUIRE) == 0);
  return 0;
}
