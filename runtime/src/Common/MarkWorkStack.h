// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_NEW_MARK_STACK_H
#define MRT_NEW_MARK_STACK_H

#include <mutex>
#include "Base/TimeUtils.h"

namespace MapleRuntime {
class BaseObject;
class MarkSweepCollector;

template<class T>
class MarkStackBuf {
constexpr static size_t MAX = 64;

public:
    MarkStackBuf<T>* next = nullptr;
    MarkStackBuf<T>* pre = nullptr;
    bool full() { return count == MAX; }

    bool empty() { return count == 0; }

    void push_back(T t)
    {
        CHECK_DETAIL(!full(), "Mark stack buffer can not be full when push back");
        stack[count] = t;
        count++;
    }

    T back()
    {
        CHECK_DETAIL(!empty(), "Mark stack buffer can not be empty when get back");
        return stack[count - 1];
    }

    void pop_back()
    {
        CHECK_DETAIL(!empty(), "Mark stack buffer can not be empty when pop back");
        count--;
    }
private:
    size_t count = 0;
    T stack[MAX];
};

template<class T>
class MarkStack {
public:
    MarkStack() = default;

    MarkStack(MarkStack&& stack)
    {
        this->h = stack.head();
        this->t = stack.tail();
        this->s = stack.size();
        stack.clean();
    }

    MarkStack(MarkStackBuf<T>* buf) : h(buf)
    {
        MarkStackBuf<T>* tmp = buf;
        while (tmp != nullptr) {
            if (tmp->next == nullptr) {
                this->t = tmp;
            }
            this->s++;
            tmp = tmp->next;
        }
    }

    ~MarkStack() { clear(); }

    MarkStackBuf<T>* head() { return this->h; }
    const MarkStackBuf<T>* head() const { return this->h; }

    MarkStackBuf<T>* tail() { return this->t; }
    const MarkStackBuf<T>* tail() const { return this->t; }

    size_t size() { return this->s; }
    size_t size() const { return this->s; }

    size_t BufferCount() const { return this->s; }

    size_t MetadataBytes() const { return this->s * sizeof(MarkStackBuf<T>); }

    bool empty()
    {
        return this->s == 0 && this->h == nullptr && this->t == nullptr;
    }

    bool empty() const
    {
        return this->s == 0 && this->h == nullptr && this->t == nullptr;
    }

    void clear()
    {
        MarkStackBuf<T>* cur = this->h;
        this->h = nullptr;
        this->t = nullptr;
        this->s = 0;
        while (cur != nullptr) {
            MarkStackBuf<T>* next = cur->next;
            delete cur;
            cur = next;
        }
    }

    void push_back(T value)
    {
        if (this->t == nullptr || this->t->full()) {
            push(new MarkStackBuf<T>());
        }
        this->t->push_back(value);
    }

    T back()
    {
        if (this->t == nullptr) {
            return nullptr;
        }
        return this->t->back();
    }

    void pop_back()
    {
        CHECK_DETAIL(!empty(), "Mark stack can not be empty when pop back");
        this->t->pop_back();
        if (this->t->empty()) {
            auto tmp = pop();
            delete tmp;
            tmp = nullptr;
        }
    }

    void insert(MarkStack<T>& stack)
    {
        if (stack.empty()) {
            return;
        }
        if (this->h == nullptr) {
            this->h = stack.head();
            this->t = stack.tail();
            this->s = stack.size();
            stack.clean();
            return;
        }
        this->t->next = stack.head();
        stack.head()->pre = this->t;
        this->t = stack.tail();
        this->s += stack.size();
        stack.clean();
    }

    MarkStackBuf<T>* split(size_t splitNum)
    {
        if (splitNum == 0 || s == 0) {
            return nullptr;
        }
        auto res = this->h;
        size_t num = 0;
        while (num < splitNum) {
            if (num == splitNum - 1) {
                auto tmp = this->h;
                this->h = this->h->next;
                if (this->h != nullptr) {
                    this->h->pre = nullptr;
                }
                tmp->next = nullptr;
            } else {
                this->h = this->h->next;
            }
            num++;
            this->s--;
            if (this->h == nullptr) {
                this->t = nullptr;
                return res;
            }
        }
        return res;
    }
private:
    void push(MarkStackBuf<T>* buf)
    {
        if (empty()) {
            this->h = buf;
            this->t = buf;
            this->s++;
            return;
        }
        buf->pre = this->t;
        this->t->next = buf;
        this->t = buf;
        this->s++;
    }

    MarkStackBuf<T>* pop()
    {
        if (empty()) {
            return nullptr;
        }
        MarkStackBuf<T>* res = this->t;
        this->t = this->t->pre;
        if (this->t != nullptr) {
            this->t->next = nullptr;
        } else {
            this->h = nullptr;
        }
        res->pre = nullptr;
        this->s--;
        return res;
    }

    void clean()
    {
        if (empty()) {
            return;
        }
        this->h = nullptr;
        this->t = nullptr;
        this->s = 0;
    }

    MarkStackBuf<T>* h = nullptr;
    MarkStackBuf<T>* t = nullptr;
    size_t s = 0;
};

// GreyWorkQueue is a constrained wrapper for queues that are expected to hold
// already-produced grey objects. It intentionally does not expose the raw
// MarkStack push_back API, so newly-grey production can be audited through
// collector-owned helpers such as TryEnqueueGreyObject.
class GreyWorkQueue {
public:
    using RawWorkStack = MarkStack<BaseObject*>;
    using Buffer = MarkStackBuf<BaseObject*>;

    GreyWorkQueue() = default;

    bool empty() const { return stack_.empty(); }
    size_t size() const { return stack_.size(); }
    void clear() { stack_.clear(); }

    BaseObject* back() { return stack_.back(); }
    void pop_back() { stack_.pop_back(); }

    void insert(GreyWorkQueue& queue)
    {
        stack_.insert(queue.stack_);
    }

    void insert(RawWorkStack& queue)
    {
        stack_.insert(queue);
    }

    Buffer* split(size_t splitNum)
    {
        return stack_.split(splitNum);
    }

    void AdoptSplitBuffers(Buffer* bufs)
    {
        if (bufs == nullptr) {
            return;
        }
        RawWorkStack tmp(bufs);
        stack_.insert(tmp);
    }

    void PushMovedGrey(BaseObject* obj)
    {
        stack_.push_back(obj);
    }

    RawWorkStack& UnsafeRaw() { return stack_; }
    const RawWorkStack& UnsafeRaw() const { return stack_; }

private:
    friend class MarkSweepCollector;

    void PushNewlyGrey(BaseObject* obj)
    {
        stack_.push_back(obj);
    }

    RawWorkStack stack_;
};
}
#endif // MRT_NEW_MARK_STACK_H
