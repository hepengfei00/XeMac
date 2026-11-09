#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>

#define _RTW88_COMPAT_TYPES_H
#define _RTW88_COMPAT_JIFFIES_H
#define _RTW88_IOKIT_SHIM_H
#define THREAD_UNINT 1
struct IOLock { std::mutex mutex; std::condition_variable cv; std::atomic<std::thread::id> owner; };
IOLock *IOLockAlloc() { return new IOLock; }
void IOLockLock(IOLock *l) {
    if (l->owner.load() == std::this_thread::get_id()) throw std::runtime_error("self-deadlock");
    l->mutex.lock(); l->owner = std::this_thread::get_id();
}
void IOLockUnlock(IOLock *l) { l->owner.store({}); l->mutex.unlock(); }
int IOLockSleep(IOLock *l, void *, unsigned) {
    std::unique_lock<std::mutex> held(l->mutex, std::adopt_lock);
    l->owner.store({}); l->cv.wait(held); l->owner = std::this_thread::get_id();
    held.release(); return 0;
}
void IOLockWakeup(IOLock *l, void *, bool) { l->cv.notify_all(); }
void IOSleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
unsigned long test_jiffies() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#define jiffies test_jiffies()
bool time_after_eq(unsigned long a, unsigned long b) { return (long)(a-b) >= 0; }
#include COMPLETION_HEADER

int main(int argc, char **) {
    completion c; init_completion(&c);
    assert(wait_for_completion_timeout(&c, 0) == 0);
    bool leaked = c.lock->owner.load() == std::this_thread::get_id();
    if (argc > 1) {
        assert(leaked);
        bool reproduced = false;
        try { reinit_completion(&c); } catch (const std::runtime_error &) { reproduced = true; }
        assert(reproduced);
        IOLockUnlock(c.lock); delete c.lock;
        std::cout << "ORIGINAL_TIMEOUT_LOCK_LEAK_AND_SELF_DEADLOCK_REPRODUCED\n";
        return 0;
    }
    assert(!leaked);
    for (unsigned n = 0; n < 20; ++n) {
        assert(wait_for_completion_timeout(&c, 0) == 0);
        reinit_completion(&c); complete(&c);
        assert(wait_for_completion_timeout(&c, 0) == 1 && c.done == 0);
    }
    complete_all(&c);
    assert(wait_for_completion_timeout(&c, 0) == 1 && c.done == ~0u);
    assert(wait_for_completion_timeout(&c, 0) == 1 && c.done == ~0u);
    reinit_completion(&c);
    std::thread producer([&] { IOSleep(2); complete(&c); });
    assert(wait_for_completion_timeout(&c, 100) == 1);
    producer.join();
    assert(!c.done && c.lock->owner.load() == std::thread::id{});
    assert(wait_for_completion_timeout(&c, 2) == 0);
    reinit_completion(&c);
    complete(&c); wait_for_completion(&c); assert(!c.done);
    delete c.lock;
    std::cout << "FIXED_TIMEOUT_REINIT_WAKEUP_AND_COMPLETE_ALL_PASSED\n";
}
