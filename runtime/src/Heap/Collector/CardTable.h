// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#ifndef MRT_CARD_TABLE_H
#define MRT_CARD_TABLE_H

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace MapleRuntime {

class CardTable {
public:
    static constexpr size_t CARD_SIZE = 512;
    static constexpr uint8_t DIRTY_CARD = 0;
    static constexpr uint8_t CLEAN_CARD = 1;

    static CardTable& GetCardTable()
    {
        static CardTable instance;
        return instance;
    }

    void Initialize(uintptr_t heapStart, size_t heapSize)
    {
        heapStart_ = heapStart;
        heapSize_ = heapSize;
        cardTableSize_ = (heapSize + CARD_SIZE - 1) / CARD_SIZE;
        cardTable_ = new std::atomic<uint8_t>[cardTableSize_];
        for (size_t i = 0; i < cardTableSize_; i++) {
            cardTable_[i].store(CLEAN_CARD, std::memory_order_relaxed);
        }
    }

    inline size_t GetCardIndex(uintptr_t addr)
    {
        if (addr < heapStart_ || addr >= heapStart_ + heapSize_) {
            return static_cast<size_t>(-1);
        }
        return (addr - heapStart_) / CARD_SIZE;
    }

    inline uintptr_t GetCardStartAddr(size_t cardIdx)
    {
        return heapStart_ + cardIdx * CARD_SIZE;
    }

    inline bool IsDirty(uintptr_t addr)
    {
        size_t idx = GetCardIndex(addr);
        if (idx == static_cast<size_t>(-1)) {
            return false;
        }
        return cardTable_[idx].load(std::memory_order_acquire) == DIRTY_CARD;
    }

    inline void MarkDirty(uintptr_t addr)
    {
        size_t idx = GetCardIndex(addr);
        if (idx != static_cast<size_t>(-1)) {
            cardTable_[idx].store(DIRTY_CARD, std::memory_order_release);
        }
    }

    inline void MarkClean(uintptr_t addr)
    {
        size_t idx = GetCardIndex(addr);
        if (idx != static_cast<size_t>(-1)) {
            cardTable_[idx].store(CLEAN_CARD, std::memory_order_release);
        }
    }

    template<typename Func>
    void IterateDirtyCards(uintptr_t startAddr, uintptr_t endAddr, Func&& func)
    {
        size_t startIdx = GetCardIndex(startAddr);
        size_t endIdx = GetCardIndex(endAddr);
        if (startIdx == static_cast<size_t>(-1) || endIdx == static_cast<size_t>(-1)) {
            return;
        }
        for (size_t i = startIdx; i <= endIdx; i++) {
            if (cardTable_[i].load(std::memory_order_acquire) == DIRTY_CARD) {
                func(GetCardStartAddr(i));
            }
        }
    }

    void ClearAll()
    {
        for (size_t i = 0; i < cardTableSize_; i++) {
            cardTable_[i].store(CLEAN_CARD, std::memory_order_relaxed);
        }
    }

    void ClearRange(uintptr_t startAddr, size_t size)
    {
        size_t startIdx = GetCardIndex(startAddr);
        size_t endIdx = GetCardIndex(startAddr + size);
        if (startIdx == static_cast<size_t>(-1) || endIdx == static_cast<size_t>(-1)) {
            return;
        }
        for (size_t i = startIdx; i <= endIdx; i++) {
            cardTable_[i].store(CLEAN_CARD, std::memory_order_release);
        }
    }

private:
    CardTable() = default;
    ~CardTable() { delete[] cardTable_; }

    uintptr_t heapStart_ = 0;
    size_t heapSize_ = 0;
    size_t cardTableSize_ = 0;
    std::atomic<uint8_t>* cardTable_ = nullptr;
};

} // namespace MapleRuntime
#endif