#!/usr/bin/env python3
"""Extract §3 into step1.1 file and replace with summary in original."""

FILE = "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/doc/Step1_分配器改造详细设计.md"
NEW_FILE = "/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/doc/Step1.1_MS_RegionInfo元数据改造设计.md"

with open(FILE, "r") as f:
    lines = f.readlines()

# §3 content: lines 249..726 (1-indexed), i.e. 248..725 (0-indexed)
# Line 247 (0-indexed) is "---" separator before §3
# Line 726 (0-indexed) is "---" separator after §3, before §4
s3_start = 248  # 0-indexed, line 249 = "## 3. MS RegionInfo ..."
s3_end = 725    # 0-indexed, exclusive; line 725 is last content line of §3

s3_content = lines[s3_start:s3_end]

# Write new file with header
with open(NEW_FILE, "w") as f:
    f.write("# Step 1.1 — MS RegionInfo 元数据改造设计\n\n")
    f.write("> 本文档从 [Step1_分配器改造详细设计.md](Step1_分配器改造详细设计.md) §3 拆分而来。\n\n")
    f.write("---\n\n")
    # Write §3 content, converting ## 3. to ## (top-level in new file)
    for line in s3_content:
        f.write(line)

print(f"Created {NEW_FILE} with {len(s3_content)} lines")

# Replace §3 in original with summary + link
summary = """## 3. MS RegionInfo 元数据改造设计

> **详细设计文档**：[Step1.1_MS_RegionInfo元数据改造设计.md](Step1.1_MS_RegionInfo元数据改造设计.md)

本节设计面向 Mark-Sweep 的精简元数据 `MSRegionInfo`（64 bytes），替代 CopyGC 的 `UnitMetadata`（~120 bytes）。核心要点：

- **UnitInfo 不再需要**：CopyGC 的三大角色（地址反查、Ghost Region、多 Unit 拼接）在 MS 模式下全部消失
- **保留堆前方镜像布局**：O(1) 查找、cache 友好、零额外分配，粒度改为 per-Region（32KB）
- **MSRegionInfo 64 bytes**：删除 ~56B CopyGC 字段（routeInfo、Ghost 备份、rwLock 等），新增 bitmap 指针、sizeClass、sweepState
- **Bitmap 替代 free-list**：allocBitmap + markBitmap，1 bit/slot，sweep = `~markBitmap`，CTZ 分配 O(1)
- **Region 大小 32KB**：8192 Region/256MB 堆，markBitmap 512B/Region，大对象阈值 16KB
- **MSRegionType 简化**：5 种状态（FREE / BUMP / PARTIAL / FULL / LARGE）替代 CopyGC 15+ 种

"""

new_lines = lines[:s3_start] + [summary] + lines[s3_end:]

with open(FILE, "w") as f:
    f.writelines(new_lines)

print(f"Updated {FILE}: {len(lines)} -> {len(new_lines)} lines")
