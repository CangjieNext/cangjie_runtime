#include <cassert>
#include <cstddef>
#include <cstdint>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MAllocator/MSBitmapArena.h"
#include "src/Heap/MAllocator/MSRegionInfo.h"
#include "src/Heap/MSGCFastPath.h"

namespace MapleRuntime {

extern "C" MSGCFastPathState CJ_MRT_MSGCFastPathState = {};

} // namespace MapleRuntime

int main() {
  using namespace MapleRuntime;

  static_assert(sizeof(MSGCFastPathState) == 64, "MSGC fast-path state size");
  static_assert(offsetof(MSGCFastPathState, currentMarkEpoch) == 0,
                "mark epoch offset");
  static_assert(offsetof(MSGCFastPathState, heapReservedStart) == 8,
                "heap start offset");
  static_assert(offsetof(MSGCFastPathState, heapReservedEnd) == 16,
                "heap end offset");
  static_assert(offsetof(MSGCFastPathState, regionInfoArray) == 24,
                "region info offset");
  static_assert(offsetof(MSGCFastPathState, allocPlaneBase) == 32,
                "alloc plane offset");
  static_assert(offsetof(MSGCFastPathState, markPlaneBase) == 40,
                "mark plane offset");
  static_assert(offsetof(MSGCFastPathState, scanPlaneBase) == 48,
                "scan plane offset");
  static_assert(offsetof(MSGCFastPathState, reservedAbi) == 56,
                "reserved ABI offset");
  static_assert(sizeof(MSRegionInfo) == 64, "RegionInfo cache-line size");
  static_assert(alignof(MSRegionInfo) == 64, "RegionInfo cache-line alignment");
  static_assert(offsetof(MSRegionInfo, allocPtr) == 0, "allocPtr offset");
  static_assert(offsetof(MSRegionInfo, regionEnd) == 8, "regionEnd offset");
  static_assert(offsetof(MSRegionInfo, centralNextRegionIdx) == 16,
                "central link offset");
  static_assert(offsetof(MSRegionInfo, markTaskNextRegionIdx) == 20,
                "mark-task link offset");
  static_assert(offsetof(MSRegionInfo, allocBitmap) == 24,
                "allocation bitmap pointer offset");
  static_assert(offsetof(MSRegionInfo, collectEpoch) == 32,
                "collect epoch offset");
  static_assert(offsetof(MSRegionInfo, control) == 36, "control offset");
  static_assert(offsetof(MSRegionInfo, allocSummary) == 40,
                "allocation summary offset");
  static_assert(offsetof(MSRegionInfo, pendingWordSummary) == 48,
                "pending-word summary offset");
  static_assert(offsetof(MSRegionInfo, reserved32) == 56,
                "legacy free-slot-count ABI tombstone offset");
  static_assert(offsetof(MSRegionInfo, allocMode) == 60,
                "allocation mode offset");
  static_assert(offsetof(MSRegionInfo, sizeClass) == 62,
                "size class offset");
  static_assert(offsetof(MSRegionInfo, layout) == 63, "layout offset");

  PublishMSGCFastPathHeapRange(0x100000000ULL, 0x200000000ULL);
  PublishMSGCFastPathRegionInfoArray(0x300000000ULL);
  PublishMSGCFastPathPlaneBases(0x400000000ULL, 0x500000000ULL,
                               0x600000000ULL);
  PublishMSGCFastPathMarkEpoch(7);

  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.heapReservedStart,
                         __ATOMIC_ACQUIRE) == 0x100000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.heapReservedEnd,
                         __ATOMIC_ACQUIRE) == 0x200000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.regionInfoArray,
                         __ATOMIC_ACQUIRE) == 0x300000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.allocPlaneBase,
                         __ATOMIC_ACQUIRE) == 0x400000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.markPlaneBase,
                         __ATOMIC_ACQUIRE) == 0x500000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.scanPlaneBase,
                         __ATOMIC_ACQUIRE) == 0x600000000ULL);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.currentMarkEpoch,
                         __ATOMIC_ACQUIRE) == 7);

  alignas(64) MSRegionInfo regions[2] {};
  alignas(64) uint64_t allocPlane[2 * MSBitmapArena::kMaxBitmapWords] {};
  alignas(64) uint64_t markPlane[2 * MSBitmapArena::kMaxBitmapWords] {};
  alignas(64) uint64_t scanPlane[2 * MSBitmapArena::kMaxBitmapWords] {};
  PublishMSGCFastPathRegionInfoArray(reinterpret_cast<uintptr_t>(regions));
  PublishMSGCFastPathPlaneBases(0x700000000ULL,
                               reinterpret_cast<uintptr_t>(markPlane),
                               reinterpret_cast<uintptr_t>(scanPlane));
  regions[0].allocBitmap = allocPlane;
  regions[1].allocBitmap = allocPlane + MSBitmapArena::kMaxBitmapWords;
  assert(regions[0].GetAllocBitmap() == allocPlane);
  assert(regions[1].GetAllocBitmap() ==
         allocPlane + MSBitmapArena::kMaxBitmapWords);
  assert(regions[1].GetMarkBitmap() ==
         markPlane + MSBitmapArena::kMaxBitmapWords);
  assert(regions[1].GetScanBitmap() ==
         scanPlane + MSBitmapArena::kMaxBitmapWords);

  PublishMSGCFastPathRegionInfoArray(0);
  PublishMSGCFastPathPlaneBases(0, 0, 0);
  PublishMSGCFastPathMarkEpoch(0);
  assert(__atomic_load_n(&CJ_MRT_MSGCFastPathState.regionInfoArray,
                         __ATOMIC_ACQUIRE) == 0);
  return 0;
}
