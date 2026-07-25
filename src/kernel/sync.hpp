#pragma once

#include <stdint.h>

namespace sync {

inline uint64_t disable_interrupts() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

inline void restore_interrupts(uint64_t flags) {
    if ((flags & (1ull << 9)) != 0) {
        asm volatile("sti" ::: "memory");
    }
}

class SpinLock {
public:
    void lock() {
        while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
            asm volatile("pause");
        }
    }
    bool try_lock() { return !__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE); }
    void unlock() { __atomic_clear(&locked_, __ATOMIC_RELEASE); }
private:
    volatile bool locked_{false};
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    SpinLock& lock_;
};

class IrqLockGuard {
public:
    explicit IrqLockGuard(SpinLock& lock) : lock_(lock), flags_(disable_interrupts()) {
        lock_.lock();
    }
    ~IrqLockGuard() { lock_.unlock(); restore_interrupts(flags_); }
    IrqLockGuard(const IrqLockGuard&) = delete;
    IrqLockGuard& operator=(const IrqLockGuard&) = delete;
private:
    SpinLock& lock_;
    uint64_t flags_;
};

}  // namespace sync
