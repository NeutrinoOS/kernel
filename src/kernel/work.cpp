#include "kernel/work.hpp"

#include "kernel/error.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/sync.hpp"

namespace {

constexpr size_t kQueueSize = 64;
struct Item { work::Handler handler; void* context; };
Item g_queue[kQueueSize]{};
size_t g_head = 0;
size_t g_tail = 0;
size_t g_count = 0;
size_t g_active = 0;
sync::SpinLock g_lock;
bool g_registered = false;

void service() {
    for (;;) {
        Item item{};
        {
            sync::IrqLockGuard guard(g_lock);
            KERNEL_ASSERT_MSG(g_head < kQueueSize,
                              "work queue head is out of bounds");
            KERNEL_ASSERT_MSG(g_tail < kQueueSize,
                              "work queue tail is out of bounds");
            KERNEL_ASSERT_MSG(g_count <= kQueueSize,
                              "work queue count is out of bounds");
            if (g_count == 0) return;
            item = g_queue[g_head];
            KERNEL_ASSERT_MSG(item.handler != nullptr,
                              "work queue contains a null handler");
            g_head = (g_head + 1) % kQueueSize;
            --g_count;
            ++g_active;
        }
        item.handler(item.context);
        {
            sync::IrqLockGuard guard(g_lock);
            KERNEL_ASSERT_MSG(g_active != 0,
                              "work queue active count underflow");
            --g_active;
        }
    }
}

}  // namespace

namespace work {

bool schedule(Handler handler, void* context) {
    if (handler == nullptr) return false;
    {
        sync::IrqLockGuard guard(g_lock);
        KERNEL_ASSERT_MSG(g_head < kQueueSize,
                          "work queue head is out of bounds");
        KERNEL_ASSERT_MSG(g_tail < kQueueSize,
                          "work queue tail is out of bounds");
        KERNEL_ASSERT_MSG(g_count <= kQueueSize,
                          "work queue count is out of bounds");
        if (g_count == kQueueSize) return false;
        if (!g_registered) {
            if (!scheduler::register_poll(service)) return false;
            g_registered = true;
        }
        g_queue[g_tail] = Item{handler, context};
        g_tail = (g_tail + 1) % kQueueSize;
        ++g_count;
    }
    return true;
}

bool busy() {
    sync::IrqLockGuard guard(g_lock);
    return g_count != 0 || g_active != 0;
}

void wait() {
    while (busy()) asm volatile("pause");
}

}  // namespace work
