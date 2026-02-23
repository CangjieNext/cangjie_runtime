# LOS Compaction 调试检查点（2026-02-22）

## 当前状态
- 并发搬移主线已接入：
  - barrier assist 前驱推进（访问中间对象时可触发依赖安装）
  - compaction plan 并发访问加锁（快照读取）
  - finalize 与 pending plan 生命周期已形成闭环
- 之前频发的 `failed to add dirty units` 已做止血处理（重复 free 区间不再直接 fatal）。
- 单轮高压验证结果：**通过**（`EXIT:0`，`=== NO CRASH ===`）。

## 最近一次有效验证
- 用例：`gc/main_crash_repro`
- 运行参数：`cjEnableLOSCompact=1 cjHeapSize=64mb`
- 诊断日志：`gc/diag_one_round_try.log`
- 结果：
  - 程序输出：`=== NO CRASH ===`
  - 退出码：`0`

## 2026-02-22 最新增量
- 追加了 2 轮短回归尝试：
  - `diag_next_2.log` 对应一轮完整通过（stdout 结尾 `=== NO CRASH ===`）。
  - `diag_next_1.log` 对应会话中断场景（日志有 GC 进展，但 stdout 为空，未形成完整结束标记）。
- 当前未再次观察到 `failed to add dirty units` fatal。
- 现阶段判断：稳定性较昨日提升，但仍需在“无中断前提”下做 2~5 轮连续回归确认。

## 本轮关键变更（高层）
- LOS 回收/compaction 路径的并发一致性修正。
- free tree 重复插入导致的 fatal 改为可恢复处理（避免同一 free 区间重复回收直接中止进程）。
- 设计文档已同步到“并发搬移 + barrier assist”方案。

## 明天建议第一步
1. 先跑 2 轮短回归确认非偶发：
   - `source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/envsetup.sh`
   - `cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc`
   - `for i in 1 2; do env MRT_REPORT=./diag_next_${i}.log cjEnableLOSCompact=1 cjHeapSize=64mb ./main_crash_repro; echo RUN-$i:$?; done`
2. 若稳定，再扩到 5 轮；若失败，优先看是否再次出现 `failed to add dirty units`。

## 当前建议（今天继续时）
1. 用前台串行命令重新做 2 轮（避免后台会话中断噪声）：
  - `for i in 1 2; do env MRT_REPORT=./diag_serial_${i}.log cjEnableLOSCompact=1 cjHeapSize=64mb ./main_crash_repro; echo RUN-$i:$?; done`
2. 若 2/2 均为 0，再做 5 轮；若有失败，优先抓失败轮的 `diag_serial_i.log` 与 stdout 尾部。

## 关注点
- 目前 LOS 压力仍高（`alloc fail/fallback` 数值较大），后续可继续优化 compaction 触发与回收效率。
- 当前目标先稳住 correctness，再做性能和碎片优化。

---

## 2026-02-23 Shrink / RESERVE 归还验证

### 验证目标
用户提出 4 个验证点：
1. MAIN 是否低地址优先分配
2. MAIN 抢到 RESERVE 空间后是否还回去
3. LOS 在 OOM 时是否整理回缩（CompactLOS）
4. RESERVE 空间是否还回去

### 测试用例
- `gc/gc_shrink_verify.cj` — 6 阶段测试：低地址优先→填满LOS→填满MAIN借RESERVE→释放归还→LOS碎片化→最终验证
- `gc/run.sh shrink` — 双配置运行：
  - **Run A**: `cjHeapSize=64mb cjMainRatio=3 cjReserveRatio=5 cjLOSRatio=2 cjEnableLOSCompact=1` — LOS开启，测试LOS借用+CompactLOS
  - **Run B**: `cjHeapSize=64mb cjMainRatio=3 cjReserveRatio=7 cjLOSRatio=0` — LOS关闭，测试MAIN→RESERVE借/还

### 运行时构建
- `python3 build.py build --target native --build-type release -v 0.0.1`
- dylib 已替换到 SDK 路径

### Run A 结果（LOS 开启）

| 指标 | 值 |
|------|-----|
| 空间分区 | MAIN=19.2MB, RESERVE=32MB, LOS=12.8MB |
| LOS→RESERVE 借用 | losBorrowed 增长到 24MB, losBorrowCount=11 |
| MAIN→RESERVE 借用 | mainBorrowed=0（LOS 吸收了所有大对象） |
| CompactLOS 触发 | **5 次**（之前 analyze_log grep 关键字有误，漏了） |
| compactShrunk | 6.8MB → 6.6MB → 7.4MB → 2.8MB → ... |
| CompactLOS 耗时 | 约 7-9ms 每次 |
| ShrinkInactiveZone | 未触发（设计限制，见下） |
| RESERVE losReturned | **0**（CompactLOS 释放了空间但 watermark 合并失败） |
| AllocLarge MAIN fallback | 0（LOS + RESERVE borrowing 吸收了所有大对象） |

### Run B 结果（LOS 关闭，cjLOSRatio=0）

| 指标 | 值 |
|------|-----|
| 空间分区 | MAIN=19.2MB, RESERVE=44.8MB, LOS=0 |
| MAIN→RESERVE 借用 | **mainBorrowed=4,718,592 B (4.5MB)**, reserveBorrow=2 ✅ |
| MAIN 大对象分配 | 89 个 128KB chunk 成功进入 MAIN（large-object regions 89, 17.5MB） |
| **崩溃** | Phase 2 分配第 90 个 128KB chunk 时 SIGSEGV (`addr: 0x841580689`) |
| 崩溃原因 | MAIN 17.5MB + RESERVE 4.5MB ≈ 22MB 被占满，OOM-GC 只回收 2.8MB，后续分配损坏 |
| MAIN 归还 | **未测试**（崩溃在归还阶段之前） |

### 验证结论

| # | 验证点 | 结果 | 说明 |
|---|--------|------|------|
| 1 | MAIN 低地址优先 | ✅ 确认 | `lowAddressFirst=true`，inactiveZone 从低到高增长 |
| 2 | MAIN 借 RESERVE 后归还 | ⚠️ 部分 | 借用 ✅ (4.5MB)；归还未测（Run B 崩溃） |
| 3 | LOS OOM 整理回缩 | ✅ 确认 | CompactLOS 触发 5 次，碎片有效整理 |
| 4 | RESERVE 空间归还 | ❌ 未归还 | losReturned=0, mainReturned=0 |

### 发现的问题（待解决）

#### P1: RESERVE 归还机制失效（losReturned 始终为 0）
- **现象**: CompactLOS 执行 5 次，compactShrunk 累计释放数十 MB，但 `losReturned=0`
- **根因分析**: CompactLOS 的 `ShrinkToReserve()` 调用 `ReturnFromLOS()` → `SetBitmapRange()` → `CoalesceLOSWatermark()`。
  但 `CoalesceLOSWatermark` 从 `losWatermark`（高端）向上扫描连续返回位，而 CompactLOS 释放的是低端空间（`losInactiveZone` 附近）。
  bitmap 位被设了但与 losWatermark 不连续，所以 watermark 无法推进，`losReturnBytes` 不增长。
- **关键文件**: [ReserveManager.cpp L277-310](src/Heap/Allocator/ReserveManager.cpp#L277-L310) `CoalesceLOSWatermark()`
- **修复方向**: 检查 watermark 扫描方向是否与 LOS 释放方向一致；或 ShrinkToReserve 应直接用连续地址归还而非依赖 bitmap 合并

#### P2: Run B 崩溃（cjLOSRatio=0 + 大对象分配 SIGSEGV）
- **现象**: 禁用 LOS 后，89 个 128KB 大对象占满 MAIN+RESERVE，第 90 个分配时 SIGSEGV (`0x841580689` 明显是损坏指针)
- **推测**: 大对象在纯 MAIN 模式下耗尽空间后，分配失败路径或 CopyGC 处理 MAIN 中大对象时有问题
- **关键日志**: `gc_shrink_verify_losOff.log` 末 100 行；SIGSEGV 后 `CString::CString memcpy_s failed` → SIGABRT
- **优先级**: 中（仅在 LOS 完全禁用的极端配置下触发）

#### P3: ShrinkInactiveZone 不可触发 + MAIN RESERVE 归还重构（设计限制）
- **现象**: WCollector.cpp L762 的触发条件 `inactiveZone > mainEnd` 在 M4b TakeRegion 限制下不可能为真
- **原因**: M4b 在 TakeRegion 中加了 `if (addr < mainEnd - size)` 限制，防止 inactiveZone 越过 mainEnd
- **影响**: ShrinkInactiveZone 是纯防御代码，不会在正常运行中触发
- **更深层问题**: MAIN 借 RESERVE 走的是 `BorrowForMain` → `InitRegionAt` 独立路径，与 `inactiveZone` 完全无关。RESERVE-borrowed 死区域被拦截到 `pendingReserveReturns` 中不参与 dirtyTree 复用，需要 bitmap + cooldown 机制归还，整体过于复杂且不可靠。

#### P4: analyze_log 脚本检测不全
- **现象**: `run.sh` 中的 `analyze_log` 搜索 `grep "CompactLOS done"` 和 `grep "compactions=0"` 匹配不到实际日志格式
- **修复**: 改为 `grep "compactions [1-9]"` 或直接搜索 `"LOS: compactions"` 提取数值
- **优先级**: 低（工具问题，不影响功能）

### P3 修复方案：MAIN RESERVE 归还重构（2026-02-23 设计）

#### 核心思路

将 RESERVE-borrowed 区域统一纳入 `freeRegionManager` 管理，删除 `pendingReserveReturns` 和 MAIN 侧 bitmap 机制。
ShrinkInactiveZone 增加 Phase B 扫描 RESERVE-borrowed 区域，直接推 `mainWatermark` 归还。

#### 问题分析

**问题 1：触发条件 `inactiveZone > mainEnd` 永假**

`inactiveZone` 被 TakeRegion 限制不越过 `mainEnd`，而 MAIN 借 RESERVE 走的是另一条路：
```
AllocLargeInMain → TakeRegion 返回 nullptr
  → reserveManager->BorrowForMain(regionSize)
    → InitRegionAt(borrowedAddr, ...)  // 在 RESERVE 地址空间创建 region
```
借来的 region 地址 >= mainEnd，与 `inactiveZone` 完全无关。

**修正**：触发条件改为 `resMgr->GetMainBorrowedSize() > 0`。

不能用 `addr < mainWatermark - size` 替代 TakeRegion 中的 `addr < mainEnd - size`，因为
`inactiveZone` 是 bump allocator，而 `[reserveStart, mainWatermark)` 中已有被 `BorrowForMain`
分配给具体大对象的 region，两者会地址冲突。

**问题 2：RESERVE-borrowed 死区不进 dirtyTree**

当前流程：
| 阶段 | MAIN native 区域 | RESERVE-borrowed 区域 |
|------|------------------|----------------------|
| 分配 | TakeRegion → freeRegionManager 或 inactiveZone | BorrowForMain → RESERVE 地址 |
| 死亡 | ReclaimRegion → `AddGarbageUnits`（dirtyTree） | ReclaimRegion → `pendingReserveReturns`（拦截） |
| 释放 | ReleaseRegion → `AddReleaseUnits`（releasedTree） | ReleaseRegion → `pendingReserveReturns`（拦截） |
| 复用 | TakeRegion 从 tree 取 | 不复用（或 cooldown 时回填） |

RESERVE-borrowed 死区被 `IsInReserveTerritory` 拦截，不进 freeRegionManager，无法被 TakeRegion 自然复用。

**修正**：删除 `IsInReserveTerritory` 拦截，死区直接进 dirtyTree/releasedTree。

**前提**：`freeRegionManager.Initialize(mainUnitCount)` 只为 MAIN 单元分配了 CartesianTree 节点空间。
RESERVE-borrowed 的 unitIndex >= mainUnitCount，树容量不够。需要扩容：
```cpp
freeRegionManager.Initialize(mainUnitCount + reserveUnitCount);
```
`RegionInfo::Initialize(nUnit, regionHeapStart)` 已为全部 nUnit（含 RESERVE）初始化了 metadata，
所以 RESERVE 区域的 RegionInfo 可以正常访问。

**问题 3：ShrinkInactiveZone 需要增加 Phase B**

当前 ShrinkInactiveZone 只扫描 `[regionHeapStart, inactiveZone)` — MAIN native 区域。
RESERVE-borrowed 区域进入 tree 后，ShrinkInactiveZone 需要额外扫描 `[reserveStart, mainWatermark)` 归还空间。

```
STW 进入
├── Phase A: 回缩 MAIN native 空间（已有）
│   ├── 扫描 [regionHeapStart, inactiveZone) → 找 highestLiveEnd
│   ├── 重建 freeRegionManager（仅含 highestLiveEnd 以下的 free region）
│   ├── 清理 garbageRegionList
│   └── inactiveZone = highestLiveEnd, ReleaseUnits
│
└── Phase B: 归还 RESERVE-borrowed 空间（新增）
    ├── 扫描 [reserveStart, mainWatermark) → 找 highestLiveBorrowedEnd
    │   若全部死亡 → highestLiveBorrowedEnd = reserveStart
    ├── highestLiveBorrowedEnd 以下的 free region → AddGarbageUnits 回树（可复用）
    ├── [highestLiveBorrowedEnd, oldMainWatermark) → ReleaseUnits
    ├── 调用 ReclaimMainBorrowed(highestLiveBorrowedEnd) → 直接推 mainWatermark
    └── 清理 garbageRegionList 中 >= highestLiveBorrowedEnd 的条目
STW 退出
```

公式：`newMainWatermark = highestLiveBorrowedEnd`（若无存活 → `reserveStart`）
与 LOS 侧 `newLosWatermark = min(losInactiveZone, losStart)` 完全对称。

#### bitmap 可以完全去掉

| 场景 | 当前用 bitmap | 替代方案 |
|------|-------------|---------|
| LOS CompactLOS 后归还 | ~~ReturnFromLOS + CoalesceLOSWatermark~~ | **P1 已替换**: `ReclaimLOSBorrowed` 直接推水线 ✅ |
| MAIN ShrinkInactiveZone 后归还 | ReturnFromMain (触不到) | **P3 改为**: STW Phase B 扫描 → `ReclaimMainBorrowed` 直接推水线 |
| M5 增量归还 (pendingReserveReturns) | ReturnFromMain + CoalesceMainWatermark | **P3 删除**: 死区直接进 tree + Phase B 批量归还 |

三条路径都不再需要 bitmap，`returnBitmap` 整体可删除。

#### 改动清单

| 文件 | 改动 |
|------|------|
| **ReserveManager.h** | 新增 `ReclaimMainBorrowed(uintptr_t newWatermark)` 声明；删除 `ReturnFromMain`, `ReturnFromLOS`, `SetBitmapRange`, `ClearBitmapRange`, `IsBitSet`, `CoalesceMainWatermark`, `CoalesceLOSWatermark`, `returnBitmap` 成员 |
| **ReserveManager.cpp** | 新增 `ReclaimMainBorrowed` 实现；删除 `ReturnFromMain`, `ReturnFromLOS`, `SetBitmapRange`, `ClearBitmapRange`, `CoalesceMainWatermark`, `CoalesceLOSWatermark`, bitmap 初始化代码 |
| **RegionManager.cpp Initialize** | `freeRegionManager.Initialize(mainUnitCount + reserveUnitCount)` 扩容 |
| **RegionManager.cpp ReclaimRegion/ReleaseRegion** | 删除 `IsInReserveTerritory` 拦截，死区直接进 `AddGarbageUnits`/`AddReleaseUnits` |
| **RegionManager.cpp ShrinkInactiveZone** | 增加 Phase B：扫描 `[reserveStart, mainWatermark)` → 找 highestLiveBorrowedEnd → AddGarbageUnits 低地址 free → ReleaseUnits → `ReclaimMainBorrowed` |
| **RegionManager.cpp ReturnBorrowedToReserve** | 整个函数删除 |
| **RegionManager.h** | 删除 `ReturnBorrowedToReserve` 声明, `IsInReserveTerritory`, `pendingReserveReturns`, `pendingReserveReturnsMutex` |
| **WCollector.cpp** | 触发条件改为 `resMgr->GetMainBorrowedSize() > 0`；删除 M5 块（ReturnBorrowedToReserve 调用） |
| **LOSManager.cpp** | 删除 `ReturnBorrowedToReserve`（LOS 侧 M5 增量路径） |

### P1 修复方案（2026-02-23 确定）

#### 根因

`ShrinkToReserve` 把 `losInactiveZone`（native LOS 地址 >= `losStart` = `reserveEnd`）传给
`ReserveManager::ReturnFromLOS`，后者做 `AddrToUnitIndex(addr)` 时得到的 index >= `totalReserveUnits`，
导致 `SetBitmapRange` 循环条件 `i < totalReserveUnits` 直接不满足 → bitmap 完全没写 → `CoalesceLOSWatermark`
找不到返回位 → `losReturnBytes` 始终为 0。

本质：CompactLOS 释放的是 native LOS 低端空间，地址完全在 RESERVE bitmap 范围之外，`ShrinkToReserve` 是 no-op。

#### 修复思路

CompactLOS 滑动完成后，RESERVE 借用区的对象已被搬到 native LOS 高端：
- `losInactiveZone` = `lowestLiveStart`（包含 pinned）
- RESERVE 借用区 = `[losWatermark, losStart)`

安全归还边界：
```
newLosWatermark = min(losInactiveZone, losStart)
```
- 若 `losInactiveZone >= losStart`：借用区全空 → 水位线推到 `losStart`（全部归还）
- 若 `losInactiveZone < losStart`：有 pinned 留在借用区 → 水位线推到 `losInactiveZone`（pinned 以下归还）

不需要 bitmap，直接操作 `losWatermark`。

#### 改动清单

| 文件 | 改动 |
|------|------|
| `ReserveManager.h` | 新增 `ReclaimLOSBorrowed(uintptr_t newWatermark)` 声明 |
| `ReserveManager.cpp` | 新增 `ReclaimLOSBorrowed` 实现：直接推 losWatermark + 清 LOS buffer + 更新统计 |
| `LOSManager.cpp` | CompactLOS Phase 7 替换为调用 `ReclaimLOSBorrowed`；删除 `ShrinkToReserve` 函数 |
| `LOSManager.h` | 删除 `ShrinkToReserve` 声明 |

### 后续待办
1. **P2**: 调查 `cjLOSRatio=0` 崩溃 — 可以用 debug 构建复现，抓更详细栈信息
2. **修复 analyze_log**: 更新 grep 关键字匹配实际日志格式
3. **重新验证**: 修复后重跑 `./run.sh shrink`，确认 RESERVE 归还路径打通

### 环境信息备忘
```bash
# SDK
source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/envsetup.sh

# 构建
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime
python3 build.py build --target native --build-type release -v 0.0.1

# 替换 dylib
cp output/common/common_darwin_release_aarch64/runtime/libcangjie-runtime.dylib \
   /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/runtime/lib/darwin_aarch64/libcangjie-runtime.dylib

# 编译测试
cd gc && cjc gc_shrink_verify.cj -o gc_shrink_verify

# 运行测试
./run.sh shrink 2>&1

# 日志文件
gc/gc_shrink_verify_losOn.log   # Run A (LOS开启)
gc/gc_shrink_verify_losOff.log  # Run B (LOS关闭)
```
