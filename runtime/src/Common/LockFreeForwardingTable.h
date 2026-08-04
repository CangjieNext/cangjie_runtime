#ifndef MRT_LOCK_FREE_FORWARDING_TABLE_H
#define MRT_LOCK_FREE_FORWARDING_TABLE_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>

namespace MapleRuntime {

template<typename Key, typename Value, size_t Capacity = 1024>
class LockFreeForwardingTable {
public:
    struct Entry {
        std::atomic<Key> key;
        std::atomic<Value> value;
        std::atomic<bool> valid;
    };

    LockFreeForwardingTable() {
        for (size_t i = 0; i < Capacity; i++) {
            entries_[i].valid.store(false, std::memory_order_relaxed);
        }
    }

    bool Insert(Key key, Value value) {
        size_t index = Hash(key);
        size_t start = index;
        
        do {
            Entry& entry = entries_[index];
            bool expected = false;
            
            if (entry.valid.compare_exchange_strong(expected, true, 
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                entry.key.store(key, std::memory_order_release);
                entry.value.store(value, std::memory_order_release);
                return true;
            }
            
            Key existingKey = entry.key.load(std::memory_order_acquire);
            if (existingKey == key) {
                return false;
            }
            
            index = (index + 1) % Capacity;
        } while (index != start);
        
        return false;
    }

    bool Lookup(Key key, Value& value) {
        size_t index = Hash(key);
        size_t start = index;
        
        do {
            Entry& entry = entries_[index];
            
            if (!entry.valid.load(std::memory_order_acquire)) {
                return false;
            }
            
            Key existingKey = entry.key.load(std::memory_order_acquire);
            if (existingKey == key) {
                value = entry.value.load(std::memory_order_acquire);
                return true;
            }
            
            index = (index + 1) % Capacity;
        } while (index != start);
        
        return false;
    }

    bool Update(Key key, Value newValue) {
        size_t index = Hash(key);
        size_t start = index;
        
        do {
            Entry& entry = entries_[index];
            
            if (!entry.valid.load(std::memory_order_acquire)) {
                return false;
            }
            
            Key existingKey = entry.key.load(std::memory_order_acquire);
            if (existingKey == key) {
                entry.value.store(newValue, std::memory_order_release);
                return true;
            }
            
            index = (index + 1) % Capacity;
        } while (index != start);
        
        return false;
    }

    bool Remove(Key key) {
        size_t index = Hash(key);
        size_t start = index;
        
        do {
            Entry& entry = entries_[index];
            
            if (!entry.valid.load(std::memory_order_acquire)) {
                return false;
            }
            
            Key existingKey = entry.key.load(std::memory_order_acquire);
            if (existingKey == key) {
                entry.valid.store(false, std::memory_order_release);
                return true;
            }
            
            index = (index + 1) % Capacity;
        } while (index != start);
        
        return false;
    }

private:
    size_t Hash(Key key) const {
        return static_cast<size_t>(key) % Capacity;
    }

    Entry entries_[Capacity];
};

} 

#endif 
