// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_CARTESIAN_TREE_H
#define MRT_CARTESIAN_TREE_H

#include <cstddef>
#include <cstdint>

#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "LocalDeque.h"

#if defined(MRT_DEBUG)
#define DEBUG_CARTESIAN_TREE
#endif

#ifdef DEBUG_CARTESIAN_TREE
#define CTREE_ASSERT(cond, msg) MRT_ASSERT(cond, msg)
#define CTREE_CHECK_PARENT_AND_LCHILD(n) CheckParentAndLeftChild(n)
#define CTREE_CHECK_PARENT_AND_RCHILD(n) CheckParentAndRightChild(n)
#else
#define CTREE_ASSERT(cond, msg) (void(0))
#define CTREE_CHECK_PARENT_AND_LCHILD(n) (void(0))
#define CTREE_CHECK_PARENT_AND_RCHILD(n) (void(0))
#endif

// This is an implementation of a Cartesian tree.
// This can be used in arbitrary-sized, free-list allocation algorithm.
// The use of this tree and the algorithm is inspired by
// R. Jones, A. Hosking, E. Moss. The garbage collection handbook:
// the art of automatic memory management. Chapman and Hall/CRC, 2016.
// This data structure doesn't guarantee the multi-thread safety, so the external invoker should take some
// policy to avoid competition problems.
namespace MapleRuntime {
class CartesianTree {
public:
    using Index = uint32_t; // abstract index for free memory in tree node.
    using Count = uint32_t; // corresponding memory count for this tree node.

    CartesianTree() = default;
    ~CartesianTree() = default;

    // @param base: global unit index offset. Tree-local index + base = global RegionInfo index.
    //   For MAIN space, base = 0. For LOS space, base = losUnitStart.
    void Init(size_t memoryBlockCount, Index base = 0)
    {
        baseIndex = base;
        // at most we need this many nodes
        size_t nodeCount = (memoryBlockCount >> 1) + 1;
        // calculate how much we need for native allocation
        // we might need some extra space for some temporaries, so set aside another 7 slots
        size_t nativeSize = (nodeCount + 7) * AllocUtilRndUp(sizeof(Node), alignof(Node));
        nodeAllocator.Init(ALLOCUTIL_PAGE_RND_UP(nativeSize));
        // calculate how much we need for the deque temporary
        size_t dequeSize = nodeCount * sizeof(Node*);
        sud.Init(ALLOCUTIL_PAGE_RND_UP(dequeSize));
        traversalSud.Init(sud.GetMemMap());
    }

    Index GetBaseIndex() const { return baseIndex; }

    void Fini() noexcept
    {
        ClearTree();
        sud.Fini();
        nodeAllocator.Fini();
    }

    void ClearTree()
    {
        CartesianTree::Iterator it(*this);
        Node* node = it.Next();
        while (node != nullptr) {
            DeleteNode(node);
            node = it.Next();
        }

        root = nullptr;
        totalCount = 0;
    }

    Count GetTotalCount() const { return totalCount; }

    // Return the largest contiguous free block count.
    // The tree is a max-heap on count, so the root holds the maximum.
    Count GetMaxContiguousCount() const
    {
        return root != nullptr ? root->GetCount() : 0;
    }

    // Return the number of disjoint free chunks (tree nodes).
    // Typically small (10-100), safe to call during GC dump.
    Count GetFragmentCount() const
    {
        return CountNodes(root);
    }

    // Mark all free block units in stateArray with given value.
    // Uses global unit indices (localIndex + baseIndex).
    void MarkFreeBlockUnits(uint8_t* stateArray, size_t arraySize, uint8_t value) const
    {
        MarkBlockUnits(root, baseIndex, stateArray, arraySize, value);
    }

    void IncTotalCount(Count cnt) { totalCount += cnt; }

    void DecTotalCount(Count cnt)
    {
        if (totalCount < cnt) {
            LOG(RTLOG_FATAL, "CartesianTree::DecTotalCount() Should not execute here, abort.");
        }
        totalCount -= cnt;
    }

    // Remove all free blocks whose local index >= threshold.
    // Blocks that straddle the threshold are truncated: only the portion below threshold kept.
    // Returns the total unit count removed.  Uses BST-on-index property for efficient pruning.
    // Caller must hold the tree mutex.
    Count RemoveAboveIndex(Index threshold)
    {
        Count removed = 0;
        RemoveAboveImpl(root, threshold, removed);
        totalCount -= removed;
        return removed;
    }

    // insert a node to the tree, if we find connecting nodes, we merge them
    // (the non-merging insertion is not allowed)
    // true when insertion succeeded, false otherwise
    // if [idx, idx + num) clashes with existing node, it fails
    // if num is 0U, it always fails
    bool MergeInsert(Index idx, Count num, bool refreshRegionInfo)
    {
        if (root == nullptr) {
            root = new (nodeAllocator.Allocate()) Node(idx, num, refreshRegionInfo, baseIndex);
            CTREE_ASSERT(root != nullptr, "failed to allocate a new node");
            IncTotalCount(num);
            return true;
        }

        if (num == 0) {
            return false;
        }

        return MergeInsertInternal(idx, num, refreshRegionInfo);
    }

    // find a node with at least 'num' units
    bool TakeUnits(Count num, Index& idx, bool refreshRegionInfo = true)
    {
        if (root == nullptr || num == 0) {
            return false;
        }

        return TakeUnitsImpl(num, idx, refreshRegionInfo);
    }

    // find the lowest-address node with at least 'num' units (for MAIN defragmentation)
    bool TakeUnitsLowAddr(Count num, Index& idx, bool refreshRegionInfo = true)
    {
        if (root == nullptr || num == 0) {
            return false;
        }

        return TakeUnitsLowAddrImpl(num, idx, refreshRegionInfo);
    }

    // Find the lowest-address node with at least 'num' units, whose allocation end <= upperBound.
    // Used by MAIN space in unified FreeRegionManager to respect inactiveZone boundary.
    bool TakeUnitsLowAddrBounded(Count num, Index& idx, Index upperBound, bool refreshRegionInfo = true)
    {
        if (root == nullptr || num == 0 || upperBound < num) {
            return false;
        }
        return TakeUnitsLowAddrBoundedImpl(num, idx, upperBound, refreshRegionInfo);
    }

    // Find the highest-address node with at least 'num' units, whose high-end allocation start >= lowerBound.
    // Allocates from the HIGH end of the chosen block.
    // Used by LOS space in unified FreeRegionManager to respect losInactiveZone boundary.
    bool TakeUnitsHighAddrBounded(Count num, Index& idx, Index lowerBound, bool refreshRegionInfo = true)
    {
        if (root == nullptr || num == 0) {
            return false;
        }
        return TakeUnitsHighAddrBoundedImpl(num, idx, lowerBound, refreshRegionInfo);
    }

    // Check if a high-address bounded allocation of 'num' units would succeed,
    // without actually performing the allocation.
    bool HasSuitableHighAddr(Count num, Index lowerBound) const
    {
        if (root == nullptr || num == 0) {
            return false;
        }
        return FindHighestSuitableBoundedPtr(const_cast<Node**>(&root), num, lowerBound) != nullptr;
    }

    // Remove all free blocks whose [index, index+count) overlaps with [lo, hi).
    // Blocks fully inside the range are deleted. Blocks straddling boundaries are truncated.
    // Returns total unit count removed. Used by CompactLOS rebuild.
    Count RemoveInRange(Index lo, Index hi)
    {
        if (lo >= hi) {
            return 0;
        }
        Count removed = 0;
        Index deferredIdx = 0;
        Count deferredCount = 0;
        bool hasDeferred = false;
        RemoveInRangeImpl(root, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
        totalCount -= removed;
        if (hasDeferred) {
            MergeInsert(deferredIdx, deferredCount, true);
        }
        return removed;
    }

    struct Node {
        // @param base: global unit index offset for RegionInfo operations.
        Node(Index idx, Count num, bool refreshRegionInfo, Index base = 0)
            : l(nullptr), r(nullptr), index(idx), count(num)
        {
            if (refreshRegionInfo) {
                RefreshFreeRegionInfo(base);
            }
        }

        ~Node()
        {
            l = nullptr;
            r = nullptr;
        }

        inline Index GetIndex() const { return index; }

        inline Count GetCount() const { return count; }

        inline void IncCount(Count num, bool refreshRegionInfo, Index base = 0)
        {
            count += num;
            if (refreshRegionInfo && count > 0) {
                RefreshFreeRegionInfo(base);
            }
        }

        inline void ClearCount() { count = 0; }

        inline void UpdateNode(Index idx, Count cnt, bool refreshRegionInfo, Index base = 0)
        {
            index = idx;
            count = cnt;
            if (refreshRegionInfo && cnt > 0) {
                // GetNextNeighborRegion in compact gc expects free-region metadata is always up-to-date.
                // otherwise we can ignore refreshRegionInfo.
                RefreshFreeRegionInfo(base);
            }
        }

        inline bool IsProperNode() const
        {
            Count leftCount = l == nullptr ? 0 : l->GetCount();
            Count rightCount = r == nullptr ? 0 : r->GetCount();
            return (count >= leftCount && count >= rightCount);
        }

        // @param base: global unit index offset. Node stores local index;
        //   RegionInfo is accessed with (localIndex + base).
        void RefreshFreeRegionInfo(Index base = 0);
        void ReleaseMemory(Index base = 0);

        Node* l;
        Node* r;

    private:
        Index index;
        Count count;
    };

    // Iterator is defined specifically for releasing physical pages.
    // It provides a "backwards" level-order traversal with right child node visited first
    // rather than left child node first. This behaviour avoids that physical pages for regions
    // are released and again committed for near future allocation.
    class Iterator {
    public:
        explicit Iterator(CartesianTree& tree) : localQueue(tree.traversalSud)
        {
            if (tree.root != nullptr) {
                localQueue.Push(tree.root);
            }
        }

        ~Iterator() = default;

        inline Node* Next()
        {
            if (localQueue.Empty()) {
                return nullptr;
            }
            Node* front = localQueue.Front();
            if (front->r != nullptr) {
                localQueue.Push(front->r);
            }
            if (front->l != nullptr) {
                localQueue.Push(front->l);
            }
            localQueue.PopFront();
            return front;
        }

    private:
        LocalDeque<Node*> localQueue;
    };

    // very much like forward iterator in c++.
    // traverse nodes by lvalue-reference.
    class ForwardIterator {
    public:
        explicit ForwardIterator(CartesianTree& tree) : localQueue(tree.sud)
        {
            if (tree.root != nullptr) {
                localQueue.Push(&tree.root);
            }
        }

        ~ForwardIterator() = default;

        inline Node** Next()
        {
            if (localQueue.Empty()) {
                return nullptr;
            }
            Node** topElement = localQueue.Front();
            Node* node = *topElement;
            if (node->r != nullptr) {
                localQueue.Push(&node->r);
            }
            if (node->l != nullptr) {
                localQueue.Push(&node->l);
            }
            localQueue.PopFront();
            return topElement;
        }

    private:
        LocalDeque<Node**> localQueue;
    };

    inline bool Empty() const { return root == nullptr; }
    inline const Node* RootNode() const { return root; }

    // root node records the largest block of memory.
    void ReleaseRootNode()
    {
        root->ReleaseMemory(baseIndex);
        RemoveNode(root);
    }

    using RTAllocator = RTAllocatorT<sizeof(Node), alignof(Node)>;

#ifdef DEBUG_CARTESIAN_TREE
    void DumpTree(const char* msg) const;
#endif

private:
    static Count CountNodes(const Node* n)
    {
        if (n == nullptr) {
            return 0;
        }
        return 1 + CountNodes(n->l) + CountNodes(n->r);
    }

    // Mark all free block units in a state array.
    // For each free block [idx, idx+count), sets stateArray[globalIdx] = value.
    static void MarkBlockUnits(const Node* n, Index base, uint8_t* stateArray, size_t arraySize, uint8_t value)
    {
        if (n == nullptr) {
            return;
        }
        size_t start = static_cast<size_t>(n->GetIndex()) + base;
        size_t count = static_cast<size_t>(n->GetCount());
        for (size_t i = start; i < start + count && i < arraySize; i++) {
            stateArray[i] = value;
        }
        MarkBlockUnits(n->l, base, stateArray, arraySize, value);
        MarkBlockUnits(n->r, base, stateArray, arraySize, value);
    }

    Count totalCount = 0u; // sum of all node counts.
    Index baseIndex = 0;     // global unit index offset for RegionInfo (0 for MAIN, losUnitStart for LOS)
    Node* root = nullptr;
    SingleUseDeque<Node**> sud;
    SingleUseDeque<Node*> traversalSud;
    RTAllocator nodeAllocator;

    void DeleteNode(Node* n) noexcept
    {
        if (n == nullptr) {
            return;
        }
        nodeAllocator.Deallocate(n);
    }

    // Recursively deallocate all nodes in a subtree. Returns total unit count.
    Count DeleteSubtree(Node* n)
    {
        if (n == nullptr) {
            return 0;
        }
        Count removed = n->GetCount();
        removed += DeleteSubtree(n->l);
        removed += DeleteSubtree(n->r);
        nodeAllocator.Deallocate(n);
        return removed;
    }

    // Recursive implementation of RemoveAboveIndex.
    // BST property: left child has lower index, right child has higher index.
    // Max-heap on count: parent count >= child count.
    void RemoveAboveImpl(Node*& n, Index threshold, Count& removed)
    {
        if (n == nullptr) {
            return;
        }

        Index nodeIdx = n->GetIndex();
        Index nodeEnd = nodeIdx + n->GetCount();

        if (nodeIdx >= threshold) {
            // Entire node is at or above threshold.
            // Right subtree has even higher indices — delete it entirely.
            // Left subtree may have nodes below threshold — must check.
            Node* leftChild = n->l;
            removed += n->GetCount();
            removed += DeleteSubtree(n->r);
            nodeAllocator.Deallocate(n);
            n = leftChild;
            // Continue pruning the (now-promoted) left subtree
            RemoveAboveImpl(n, threshold, removed);
        } else if (nodeEnd > threshold) {
            // Node straddles threshold: truncate to [nodeIdx, threshold)
            Count oldCount = n->GetCount();
            Count newCount = threshold - nodeIdx;
            removed += (oldCount - newCount);
            // Right subtree: all indices > nodeIdx, and since nodeEnd > threshold,
            // some or all right nodes are above threshold too. Delete right subtree entirely
            // because any right node's index >= nodeEnd > threshold would be above,
            // and nodes in [nodeIdx+count_old.. ] are above threshold.
            // Actually right children have index > nodeIdx+count, which > threshold. Delete all.
            removed += DeleteSubtree(n->r);
            n->r = nullptr;
            n->UpdateNode(nodeIdx, newCount, true, baseIndex);
            // Count decreased — may violate max-heap. Fix heap property.
            MaintainHeapPropertyForNonZeroNode(n);
        } else {
            // Entire node is below threshold. Only right subtree may contain above-threshold nodes.
            RemoveAboveImpl(n->r, threshold, removed);
        }
    }

    // the following function tries to merge new node (idx, num) with n
    enum class MergeResult {
        MERGE_SUCCESS = 0, // successfully merged with the node n
        MERGE_MISS,        // the new node (idx, num) is not connected to n, cannot merge
        MERGE_ERROR        // error, operation aborted
    };

    MergeResult MergeAt(Node& n, Index idx, Count num, bool refreshRegionInfo)
    {
        Index endIdx = idx + num;

        // try to merge the inserted node to the right of n
        if (idx == n.GetIndex() + n.GetCount()) {
            return MergeToRight(n, endIdx, num, refreshRegionInfo);
        }

        // try to merge the inserted node to the left of n
        if (endIdx == n.GetIndex()) {
            return MergeToLeft(n, idx, num, refreshRegionInfo);
        }

        return MergeResult::MERGE_MISS;
    }

    // merge free units to the right of the node. free units in the new merged node ends with endIdx,
    // and we should also try to merge the nearest right node to the new node if possible.
    MergeResult MergeToRight(Node& n, Index endIdx, Count num, bool refreshRegionInfo)
    {
        // find the nearest right n of the new merged n which ends with endIdx.
        Node* parent = &n; // the parent of the *nearest* node.
        Node* nearest = n.r;
        while (nearest != nullptr) {
            if (nearest->GetIndex() == endIdx) {
                if (nearest->l != nullptr) {
                    CTREE_ASSERT(false, "merging failed case 1");
                    return MergeResult::MERGE_ERROR;
                }
                break;
            } else if (nearest->GetIndex() < endIdx) {
                CTREE_ASSERT(false, "merging failed case 2");
                return MergeResult::MERGE_ERROR;
            } else {
                parent = nearest;
                nearest = nearest->l;
            }
        }

        n.IncCount(num, refreshRegionInfo, baseIndex);
        IncTotalCount(num);

        if (nearest != nullptr) {
            n.IncCount(nearest->GetCount(), refreshRegionInfo, baseIndex);

            // now the node doesn't have left child, so we can fast remove it.
            if (parent == &n) {
                n.r = nearest->r;
            } else {
                parent->l = nearest->r;
            }
            nodeAllocator.Deallocate(nearest);
        }
        CTREE_CHECK_PARENT_AND_RCHILD(&n);
        return MergeResult::MERGE_SUCCESS;
    }

    // merge free units to the left of the node n. free units in the new merged node starts with startIdx,
    // and we should also try to merge the nearest left node to the new merged node if possible.
    MergeResult MergeToLeft(Node& n, Index startIdx, Count num, bool refreshRegionInfo)
    {
        Node* parent = &n; // the parent of the *nearest* node.
        Node* nearest = n.l;
        while (nearest != nullptr) {
            if (nearest->GetIndex() + nearest->GetCount() == startIdx) {
                if (nearest->r != nullptr) {
                    CTREE_ASSERT(false, "merging failed case 3");
                    return MergeResult::MERGE_ERROR;
                }
                break;
            } else if (nearest->GetIndex() + nearest->GetCount() > startIdx) {
                CTREE_ASSERT(false, "merging failed case 4");
                return MergeResult::MERGE_ERROR;
            } else {
                parent = nearest;
                nearest = nearest->r;
            }
        }

        n.UpdateNode(startIdx, n.GetCount() + num, refreshRegionInfo, baseIndex);
        IncTotalCount(num);

        if (nearest != nullptr) {
            // now the node doesn't have right child, so we can fast remove it.
            if (parent == &n) {
                n.l = nearest->l;
            } else {
                parent->r = nearest->l;
            }
            n.UpdateNode(nearest->GetIndex(), n.GetCount() + nearest->GetCount(), refreshRegionInfo, baseIndex);
            nodeAllocator.Deallocate(nearest);
        }
        CTREE_CHECK_PARENT_AND_LCHILD(&n);
        return MergeResult::MERGE_SUCCESS;
    }

    // see the public MergeInsert()
    bool MergeInsertInternal(Index idx, Count num, bool refreshRegionInfo);

    // rotate the node and its left child to maintain heap property
    inline Node* RotateLeftChild(Node& n) const
    {
        Node* newRoot = n.l;
        n.l = newRoot->r;
        newRoot->r = &n;
        return newRoot;
    }

    // rotate the node and its right child to maintain heap property
    inline Node* RotateRightChild(Node& n) const
    {
        Node* newRoot = n.r;
        n.r = newRoot->l;
        newRoot->l = &n;
        return newRoot;
    }

    bool TakeUnitsImpl(Count num, Index& idx, bool refershRegionInfo)
    {
        ForwardIterator it(*this);
        Node** nodePtr = it.Next(); // pointer to root node
        if (UNLIKELY(nodePtr == nullptr)) {
            return false;
        }
        Node* node = *nodePtr;
        if (node != nullptr && node->GetCount() < num) {
            DLOG(REGION, "c-tree %p fail to take %u free units", this, num);
            return false;
        }
        Node** nextNodePtr = nullptr;
        while ((nextNodePtr = it.Next()) != nullptr) {
            Node* nextNode = *nextNodePtr;
            if (nextNode != nullptr && nextNode->GetCount() < num) {
                break;
            }

            nodePtr = nextNodePtr;
        }

        node = *nodePtr;
        idx = node->GetIndex();
        auto count = node->GetCount();

        node->UpdateNode(idx + num, count - num, refershRegionInfo, baseIndex);
        DecTotalCount(num);

        if (node->GetCount() == 0) {
            RemoveZeroNode(*nodePtr);
        } else {
            LowerNonZeroNode(*nodePtr);
        }

        CTREE_CHECK_PARENT_AND_LCHILD(*nodePtr);
        CTREE_CHECK_PARENT_AND_RCHILD(*nodePtr);

        return true;
    }

    // Find the Node** (tree-internal pointer) to the lowest-address node whose count >= num.
    // Leverages both BST-on-index (left = lower addr) and max-heap-on-count
    // (if node->count < num, no descendant can satisfy either) for O(log n).
    // Returns nullptr if no suitable node exists.
    Node** FindLowestSuitablePtr(Node** nodePtr, Count num) const
    {
        if (nodePtr == nullptr || *nodePtr == nullptr || (*nodePtr)->GetCount() < num) {
            return nullptr;
        }
        // Left subtree has strictly lower indices (addresses).
        Node** left = FindLowestSuitablePtr(&((*nodePtr)->l), num);
        if (left != nullptr) {
            return left;
        }
        // No suitable node in left subtree; current node is the lowest suitable.
        return nodePtr;
    }

    // Like TakeUnitsImpl but always allocates from the lowest-address suitable node.
    // Used by MAIN's FreeRegionManager to pack live regions toward low addresses.
    bool TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
    {
        Node** nodePtr = FindLowestSuitablePtr(&root, num);
        if (nodePtr == nullptr) {
            DLOG(REGION, "c-tree %p fail to take %u free units (low-addr)", this, num);
            return false;
        }

        Node* node = *nodePtr;
        idx = node->GetIndex();
        auto count = node->GetCount();

        node->UpdateNode(idx + num, count - num, refreshRegionInfo, baseIndex);
        DecTotalCount(num);

        if (node->GetCount() == 0) {
            RemoveZeroNode(*nodePtr);
        } else {
            LowerNonZeroNode(*nodePtr);
        }

        return true;
    }

    // Like FindLowestSuitablePtr but with an upper bound: allocation end must be <= upperBound.
    // Skips nodes whose index + num > upperBound, but still checks left subtree (lower addresses).
    Node** FindLowestSuitableBoundedPtr(Node** nodePtr, Count num, Index upperBound) const
    {
        if (nodePtr == nullptr || *nodePtr == nullptr || (*nodePtr)->GetCount() < num) {
            return nullptr;
        }
        if ((*nodePtr)->GetIndex() + num > upperBound) {
            // This node is too high; only left subtree can have lower-address candidates
            return FindLowestSuitableBoundedPtr(&((*nodePtr)->l), num, upperBound);
        }
        // Left subtree has lower indices — try it first
        Node** left = FindLowestSuitableBoundedPtr(&((*nodePtr)->l), num, upperBound);
        if (left != nullptr) {
            return left;
        }
        // Current node satisfies: count >= num && index + num <= upperBound
        return nodePtr;
    }

    // Low-addr bounded allocation: like TakeUnitsLowAddrImpl but skips blocks above upperBound.
    bool TakeUnitsLowAddrBoundedImpl(Count num, Index& idx, Index upperBound, bool refreshRegionInfo)
    {
        Node** nodePtr = FindLowestSuitableBoundedPtr(&root, num, upperBound);
        if (nodePtr == nullptr) {
            DLOG(REGION, "c-tree %p fail to take %u free units (low-addr-bounded, upper=%u)", this, num, upperBound);
            return false;
        }

        Node* node = *nodePtr;
        idx = node->GetIndex();
        auto count = node->GetCount();

        // Allocate from low end: [idx, idx+num), remainder at [idx+num, idx+count)
        node->UpdateNode(idx + num, count - num, refreshRegionInfo, baseIndex);
        DecTotalCount(num);

        if (node->GetCount() == 0) {
            RemoveZeroNode(*nodePtr);
        } else {
            LowerNonZeroNode(*nodePtr);
        }
        return true;
    }

    // Find the highest-address node that can satisfy a high-end allocation of 'num' units
    // with allocStart = (index + count - num) >= lowerBound.
    // Leverages BST (right = higher addr) and max-heap (count) for O(log n).
    // Proof that left subtree can be skipped when current node fails:
    //   - BST: left.index + left.count <= node.index
    //   - Heap: left.count <= node.count
    //   - If node.index + node.count < lowerBound + num, then
    //     left.index + left.count <= node.index < lowerBound + num, so left also fails.
    Node** FindHighestSuitableBoundedPtr(Node** nodePtr, Count num, Index lowerBound) const
    {
        if (nodePtr == nullptr || *nodePtr == nullptr || (*nodePtr)->GetCount() < num) {
            return nullptr;
        }
        Node* node = *nodePtr;
        // Right subtree has higher indices — try it first for highest address
        Node** right = FindHighestSuitableBoundedPtr(&(node->r), num, lowerBound);
        if (right != nullptr) {
            return right;
        }
        // Check current node: high-end allocation start = index + count - num >= lowerBound
        if (node->GetIndex() + node->GetCount() >= lowerBound + num) {
            return nodePtr;
        }
        // Left subtree can't satisfy (proven in comment above)
        return nullptr;
    }

    // High-addr bounded allocation: takes from the HIGH end of the chosen block.
    // allocStart = index + count - num. Remainder = [index, count - num).
    bool TakeUnitsHighAddrBoundedImpl(Count num, Index& idx, Index lowerBound, bool refreshRegionInfo)
    {
        Node** nodePtr = FindHighestSuitableBoundedPtr(&root, num, lowerBound);
        if (nodePtr == nullptr) {
            DLOG(REGION, "c-tree %p fail to take %u free units (high-addr-bounded, lower=%u)", this, num, lowerBound);
            return false;
        }

        Node* node = *nodePtr;
        auto count = node->GetCount();
        // Allocate from high end: [index+count-num, index+count), remainder [index, count-num)
        idx = node->GetIndex() + count - num;
        node->UpdateNode(node->GetIndex(), count - num, refreshRegionInfo, baseIndex);
        DecTotalCount(num);

        if (node->GetCount() == 0) {
            RemoveZeroNode(*nodePtr);
        } else {
            LowerNonZeroNode(*nodePtr);
        }
        return true;
    }

    // Recursive implementation of RemoveInRange.
    // Removes (or truncates) all nodes whose range overlaps [lo, hi).
    // Uses deferred insertion for the rare split case (node spanning entire range).
    void RemoveInRangeImpl(Node*& n, Index lo, Index hi, Count& removed,
                           Index& deferredIdx, Count& deferredCount, bool& hasDeferred)
    {
        if (n == nullptr) {
            return;
        }

        Index nodeIdx = n->GetIndex();
        Index nodeEnd = nodeIdx + n->GetCount();

        if (nodeEnd <= lo) {
            // Entirely below range — only right subtree may overlap
            RemoveInRangeImpl(n->r, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
            return;
        }

        if (nodeIdx >= hi) {
            // Entirely above range — only left subtree may overlap
            RemoveInRangeImpl(n->l, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
            return;
        }

        // Node overlaps [lo, hi)
        if (nodeIdx >= lo && nodeEnd <= hi) {
            // Fully in range: remove node
            removed += n->GetCount();
            n->ClearCount();
            Node*& leafRef = MaintainHeapPropertyForZeroNode(n);
            nodeAllocator.Deallocate(leafRef);
            leafRef = nullptr;
            // Re-enter: tree restructured, new node may be at position n
            RemoveInRangeImpl(n, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
            return;
        }

        if (nodeIdx < lo && nodeEnd > hi) {
            // Spans entire range: split into [nodeIdx, lo) and [hi, nodeEnd)
            // No other nodes can overlap [lo, hi) (BST: left < nodeIdx < lo, right > nodeEnd > hi)
            Count rightPieceCount = nodeEnd - hi;
            removed += (nodeEnd - lo); // accounts for both removed middle and right piece move
            n->UpdateNode(nodeIdx, lo - nodeIdx, true, baseIndex);
            MaintainHeapPropertyForNonZeroNode(n);
            // Defer right piece insertion to caller (avoids modifying tree from inside recursion)
            deferredIdx = hi;
            deferredCount = rightPieceCount;
            hasDeferred = true;
            return;
        }

        if (nodeIdx < lo) {
            // Straddles lo boundary: keep [nodeIdx, lo), lose [lo, nodeEnd)
            removed += (nodeEnd - lo);
            n->UpdateNode(nodeIdx, lo - nodeIdx, true, baseIndex);
            MaintainHeapPropertyForNonZeroNode(n);
            // Re-enter: right subtree nodes (with higher index) may still be in range
            RemoveInRangeImpl(n, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
            return;
        }

        // Straddles hi boundary: nodeIdx >= lo, nodeEnd > hi. Keep [hi, nodeEnd).
        removed += (hi - nodeIdx);
        n->UpdateNode(hi, nodeEnd - hi, true, baseIndex);
        MaintainHeapPropertyForNonZeroNode(n);
        // Re-enter: left subtree nodes (with lower index) may still be in range
        RemoveInRangeImpl(n, lo, hi, removed, deferredIdx, deferredCount, hasDeferred);
    }

    bool AllocateLowestAddressFromNode(Node*& node, Count count, Index& index)
    {
        Count nodeCount = node->GetCount();
        if (nodeCount < count) {
            return false;
        }

        index = node->GetIndex();
        DLOG(REGION, "c-tree %p v-alloc %u units from [%u+%u, %u)", this, count, index, nodeCount, index + nodeCount);

        node->UpdateNode(index + count, nodeCount - count, false, baseIndex);
        DecTotalCount(count);
        if (node->GetCount() == 0) {
            RemoveZeroNode(node);
        } else {
            LowerNonZeroNode(node);
        }
        return true;
    }

    // move node n down in the tree to maintain the heap property
    ATTR_NO_INLINE Node* LowerNode(Node* n)
    {
        CTREE_ASSERT(n, "lowering node failed");
        Node* tmp = nullptr;

        if (n->l != nullptr && n->l->GetCount() > n->GetCount()) {
            // this makes right tree slightly taller
            if (n->r != nullptr && n->r->GetCount() > n->l->GetCount()) {
                tmp = RotateRightChild(*n);
                tmp->l = LowerNode(tmp->l);
                CTREE_CHECK_PARENT_AND_LCHILD(tmp);
                return tmp;
            } else {
                tmp = RotateLeftChild(*n);
                tmp->r = LowerNode(tmp->r);
                CTREE_CHECK_PARENT_AND_RCHILD(tmp);
                return tmp;
            }
        }

        if (n->r != nullptr && n->r->GetCount() > n->GetCount()) {
            tmp = RotateRightChild(*n);
            tmp->l = LowerNode(tmp->l);
            CTREE_CHECK_PARENT_AND_LCHILD(tmp);
            return tmp;
        }

        return n;
    }

    // return the new position of node n.
    Node*& LowerImproperNodeOnce(Node*& n) const
    {
        CTREE_ASSERT(n, "failed to lower node once");
        if (n->l != nullptr) {
            // use the child which has the max count to instead of node.
            if (n->r != nullptr && n->r->GetCount() >= n->l->GetCount()) {
                Node* newRoot = RotateRightChild(*n);
                CTREE_CHECK_PARENT_AND_LCHILD(newRoot);
                n = newRoot;
                return newRoot->l;
            }
            Node* newRoot = RotateLeftChild(*n);
            CTREE_CHECK_PARENT_AND_RCHILD(newRoot);
            n = newRoot;
            return newRoot->r;
        }
        // the node have right child only
        if (n->r != nullptr) {
            Node* newRoot = RotateRightChild(*n);
            CTREE_CHECK_PARENT_AND_LCHILD(newRoot);
            n = newRoot;
            return newRoot->l;
        }
        return n;
    }

    // maitain the heap property for subtree with root node n. assume n->GetCount() returns 0 for now.
    // return the new position of node n.
    Node*& MaintainHeapPropertyForZeroNode(Node*& n) const
    {
        Node** nodePtr = &n;
        while ((*nodePtr)->l != nullptr || (*nodePtr)->r != nullptr) {
            nodePtr = &LowerImproperNodeOnce(*nodePtr);
        }
        return *nodePtr;
    }

    // maitain the heap property for subtree with root node n. assume n->GetCount() is less than its children.
    // return the new position of node n.
    void MaintainHeapPropertyForNonZeroNode(Node*& n) const
    {
        Node** nodePtr = &n;
        // *nodePtr won't be nullptr, don't need to check it.
        while (!(**nodePtr).IsProperNode()) {
            nodePtr = &LowerImproperNodeOnce(*nodePtr);
        }
    }

    void RemoveZeroNode(Node*& n)
    {
        Node*& nodeRef = MaintainHeapPropertyForZeroNode(n);
        CHECK_DETAIL((nodeRef->l == nullptr && nodeRef->r == nullptr), "UNEXPECT");
        nodeAllocator.Deallocate(nodeRef);
        nodeRef = nullptr;
    }

    void RemoveNode(Node*& n)
    {
        DecTotalCount(n->GetCount());
        n->ClearCount();
        RemoveZeroNode(n);
    }

    // move node n down in the tree to maintain the heap property
    void LowerNonZeroNode(Node*& n) const { MaintainHeapPropertyForNonZeroNode(n); }

#ifdef DEBUG_CARTESIAN_TREE
    inline void CheckParentAndLeftChild(const Node* n) const
    {
        if (n != nullptr) {
            const Node* l = n->l;
            if (l != nullptr) {
                CTREE_ASSERT((n->GetIndex() > (l->GetIndex() + l->GetCount())), "left child overlapped with parent");
                CTREE_ASSERT((n->GetCount() >= l->GetCount()), "left child bigger than parent");
            }
        }
    }
    inline void CheckParentAndRightChild(const Node* n) const
    {
        if (n != nullptr) {
            const Node* r = n->r;
            if (r != nullptr) {
                CTREE_ASSERT(((n->GetIndex() + n->GetCount()) < r->GetIndex()), "right child overlapped with parent");
                CTREE_ASSERT((n->GetCount() >= r->GetCount()), "right child bigger than parent");
            }
        }
    }

    inline void CheckNode(const Node* n) const
    {
        CheckParentAndLeftChild(n);
        CheckParentAndRightChild(n);
    }

    void VerifyTree()
    {
        Count total = 0;
        CartesianTree::Iterator it(*this);
        Node* node = it.Next();
        while (node != nullptr) {
            total += node->GetCount();
            CheckNode(node);
            node = it.Next();
        }

        if (total != GetTotalCount()) {
            DLOG(REGION, "c-tree %p total unit count %u (expect %u)", this, GetTotalCount(), total);
            DumpTree("internal error tree");
            LOG(RTLOG_FATAL, "CartesianTree::VerifyTree() Should not execute here, abort.");
        }
    }
#endif
};
} // namespace MapleRuntime
#endif // MRT_CARTESIAN_TREE_H
