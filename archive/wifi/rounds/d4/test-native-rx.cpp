#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "source/rtw/hal_rtw89/Rtw89NativeRxQueue.hpp"

using UInt64 = uint64_t;
constexpr int kIOReturnSuccess = 0, RTW89_FLAG_ENABLED = 1;
struct OSObject {};
using IOSimpleLock = std::mutex;
IOSimpleLock *IOSimpleLockAlloc() { return new IOSimpleLock; }
void IOSimpleLockFree(IOSimpleLock *p) { delete p; }
void IOSimpleLockLock(IOSimpleLock *p) { p->lock(); }
void IOSimpleLockUnlock(IOSimpleLock *p) { p->unlock(); }
bool failEventAllocation = false;
struct RTWCore89;
struct IOInterruptEventSource;
thread_local bool inNativeGate = false;
struct GateScope {
    std::unique_lock<std::recursive_mutex> held;
    bool previous;
    explicit GateScope(std::recursive_mutex &gate) : held(gate), previous(inNativeGate) { inNativeGate = true; }
    ~GateScope() { inNativeGate = previous; }
};
struct WorkLoop {
    std::recursive_mutex gate;
    IOInterruptEventSource *event = nullptr;
    bool failAdd = false;
    bool inGate() { return inNativeGate; }
    int addEventSource(IOInterruptEventSource *);
    void removeEventSource(IOInterruptEventSource *);
    bool fire();
};
struct IOInterruptEventSource {
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);
    OSObject *owner; Action action; WorkLoop *loop = nullptr;
    std::atomic<unsigned> pending {0};
    std::atomic<bool> enabled {false};
    static IOInterruptEventSource *interruptEventSource(OSObject *o, Action a, void *) {
        if (failEventAllocation) return nullptr;
        auto *p = new IOInterruptEventSource; p->owner = o; p->action = a; return p;
    }
    void enable() { enabled = true; pending++; }
    void disable() { enabled = false; }
    // Deliberately never enters the native gate from a producer.
    void interruptOccurred(void *, void *, int) { pending++; }
    WorkLoop *getWorkLoop() { return loop; }
    void release() { delete this; }
};
int WorkLoop::addEventSource(IOInterruptEventSource *p) {
    GateScope scope(gate); if (failAdd) return -1; event = p; p->loop = this; return 0;
}
void WorkLoop::removeEventSource(IOInterruptEventSource *p) {
    GateScope scope(gate); assert(event == p); event = nullptr; p->loop = nullptr;
}
bool WorkLoop::fire() {
    GateScope scope(gate);
    if (!event || !event->enabled || !event->pending.exchange(0)) return false;
    event->action(event->owner, event, 1); return true;
}

struct sk_buff { size_t len; unsigned id; };
struct Mbuf { unsigned id; };
struct mbuf_list { std::vector<Mbuf *> entries; unsigned ml_len = 0; };
void ml_init(mbuf_list *p) { p->entries.clear(); p->ml_len = 0; }
bool ml_empty(mbuf_list *p) { return p->ml_len == 0; }
void ml_purge(mbuf_list *p) { for (auto *m : p->entries) delete m; ml_init(p); }
struct Ifnet { bool if_snd = true; };
void ifq_clr_oactive(bool *p) { *p = false; }
struct FakeIC { Ifnet ic_if; };
struct FakeSC { FakeIC sc_ic; unsigned sc_flags = 1; };
struct Rtw89LinuxContext {
    void *nativeOwner = nullptr;
    bool (*queueReceive)(void *, void *, size_t) = nullptr;
};
std::atomic<unsigned> nextID {0}, allocated {0}, freed {0};
std::mutex freedLock;
std::unordered_map<unsigned, unsigned> frees;
std::vector<unsigned> delivered;
sk_buff *packet(size_t length = 1500) {
    allocated++; return new sk_buff{length, ++nextID};
}
void rtw89LinuxReleaseReceivedFrame(void *p) {
    assert(p); auto *skb = static_cast<sk_buff *>(p);
    { std::lock_guard<std::mutex> lock(freedLock); assert(++frees[skb->id] == 1); }
    freed++; delete skb;
}
void kfree_skb(sk_buff *p) { if (p) rtw89LinuxReleaseReceivedFrame(p); }
void if_input(Ifnet *, mbuf_list *p) {
    assert(inNativeGate);
    for (auto *m : p->entries) { delivered.push_back(m->id); delete m; }
    ml_init(p);
}
void rtw89LinuxProcessReceivedFrame(Rtw89LinuxContext *, void *);

struct RTWCore89 : OSObject {
    WorkLoop loop;
    FakeSC sc;
    Rtw89LinuxContext linuxContext;
    IOSimpleLock *nativeRxLock = nullptr;
    IOInterruptEventSource *nativeRxEvent = nullptr;
    Rtw89NativeRx::Queue nativeRxQueue;
    mbuf_list *nativeRxBatch = nullptr;
    UInt64 nativeRxBatches = 0, nativeRxFlushes = 0, nativeRxDelivered = 0;
    UInt64 nativeRxPurged = 0, nativeRxAllocDrops = 0;
    volatile UInt64 nativeRxGateViolations = 0;
    unsigned txCalls = 0, statsCalls = 0;
    bool changeEpochDuringReceive = false;
    WorkLoop *getMainWorkLoop() { return &loop; }
    bool initializeNativeRx();
    static bool enqueueNativeRx(void *, void *, size_t);
    bool resumeNativeRx();
    void pauseNativeRx();
    void stopNativeRx();
    void requestNativeTx(bool);
    void requestNativeRxStatistics();
    static void nativeRxAction(OSObject *, IOInterruptEventSource *, int);
    void drainNativeRx();
    void publishNativeRxStatistics() { assert(inNativeGate); statsCalls++; }
    static void start(Ifnet *) { assert(inNativeGate); }
    explicit RTWCore89(bool setup = true) {
        linuxContext.nativeOwner = this; linuxContext.queueReceive = enqueueNativeRx;
        if (setup) { assert(initializeNativeRx()); GateScope gate(loop.gate); assert(resumeNativeRx()); }
    }
    ~RTWCore89() { stopNativeRx(); IOSimpleLockFree(nativeRxLock); }
    void drainAll() { for (unsigned n = 0; n < 10000 && loop.fire(); ++n) {} }
};

#include "source/rtw/hal_rtw89/RTWCore89NativeRx.inc"
// Generated from the production bridge; tests exercise its ownership contract.
#include "bridge-enqueue-under-test.inc"

void rtw89LinuxProcessReceivedFrame(Rtw89LinuxContext *context, void *p) {
    auto *owner = static_cast<RTWCore89 *>(context->nativeOwner);
    assert(owner->loop.inGate()); assert(owner->nativeRxBatch);
    auto *skb = static_cast<sk_buff *>(p);
    owner->nativeRxBatch->entries.push_back(new Mbuf{skb->id});
    owner->nativeRxBatch->ml_len++;
    if (owner->changeEpochDuringReceive) {
        owner->changeEpochDuringReceive = false;
        owner->pauseNativeRx(); assert(owner->resumeNativeRx());
        queueReceiveFrame(context, packet());
        owner->requestNativeTx(true);
    }
    rtw89LinuxReleaseReceivedFrame(p);
}

int main() {
    {
        RTWCore89 driver(false); failEventAllocation = true;
        assert(!driver.initializeNativeRx()); failEventAllocation = false;
        queueReceiveFrame(&driver.linuxContext, packet());
        driver.stopNativeRx();
        std::cout << "PASS event allocation failure ownership\n";
    }
    {
        RTWCore89 driver(false); driver.loop.failAdd = true;
        assert(!driver.initializeNativeRx());
        assert(!driver.nativeRxEvent && !driver.loop.event);
        queueReceiveFrame(&driver.linuxContext, packet());
        std::cout << "PASS event registration failure ownership\n";
    }
    {
        RTWCore89 driver;
        queueReceiveFrame(&driver.linuxContext, packet(9));
        assert(driver.nativeRxQueue.statistics().invalidDrops == 1);
        driver.pauseNativeRx(); queueReceiveFrame(&driver.linuxContext, packet());
        assert(driver.nativeRxQueue.statistics().closedDrops == 1);
        std::cout << "PASS invalid and paused receive rejection\n";
    }
    {
        RTWCore89 driver; delivered.clear(); std::vector<unsigned> expected;
        for (unsigned n = 0; n < 150; ++n) { auto *p = packet(); expected.push_back(p->id); queueReceiveFrame(&driver.linuxContext, p); }
        assert(delivered.empty()); driver.drainAll();
        assert(delivered == expected); assert(driver.nativeRxFlushes == 3);
        assert(driver.nativeRxGateViolations == 0);
        std::cout << "PASS FIFO ownership, 64-frame batches, single flush per batch\n";
    }
    {
        RTWCore89 driver; delivered.clear();
        for (unsigned n = 0; n <= Rtw89NativeRx::Queue::capacity; ++n) queueReceiveFrame(&driver.linuxContext, packet());
        assert(driver.nativeRxQueue.depth() == 512);
        assert(driver.nativeRxQueue.statistics().fullDrops == 1);
        driver.stopNativeRx(); assert(driver.nativeRxQueue.depth() == 0);
        assert(delivered.empty());
        std::cout << "PASS full queue drops, shutdown purge, repeated stop\n";
    }
    {
        RTWCore89 driver;
        for (unsigned n = 0; n < 65; ++n) queueReceiveFrame(&driver.linuxContext, packet(65535));
        assert(driver.nativeRxQueue.depth() == 64);
        assert(driver.nativeRxQueue.queuedBytes() <= Rtw89NativeRx::Queue::byteLimit);
        assert(driver.nativeRxQueue.statistics().fullDrops == 1);
        std::cout << "PASS independent byte limit\n";
    }
    {
        RTWCore89 driver; delivered.clear();
        for (unsigned n = 0; n < 4; ++n) queueReceiveFrame(&driver.linuxContext, packet());
        { GateScope gate(driver.loop.gate); driver.pauseNativeRx();
          assert(driver.nativeRxQueue.depth() == 0); driver.resumeNativeRx(); }
        auto *fresh = packet(); auto id = fresh->id; queueReceiveFrame(&driver.linuxContext, fresh);
        driver.drainAll(); assert(delivered == std::vector<unsigned>{id});
        assert(driver.nativeRxQueue.statistics().staleDrops == 4);
        std::cout << "PASS power/session generation isolation\n";
    }
    {
        RTWCore89 driver; delivered.clear();
        driver.changeEpochDuringReceive = true;
        queueReceiveFrame(&driver.linuxContext, packet());
        driver.drainAll(); assert(delivered.size() == 1);
        assert(driver.nativeRxPurged == 1);
        assert(!driver.sc.sc_ic.ic_if.if_snd);
        assert(!driver.nativeRxQueue.needsService());
        std::cout << "PASS reentrant session change purges old batch and retains new work\n";
    }
    {
        RTWCore89 driver;
        std::future<void> producer;
        { GateScope gate(driver.loop.gate);
          producer = std::async(std::launch::async, [&] {
            for (unsigned n = 0; n < 500; ++n) queueReceiveFrame(&driver.linuxContext, packet());
            driver.requestNativeTx(true); driver.requestNativeRxStatistics();
          });
          assert(producer.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready);
        }
        producer.get(); driver.drainAll(); assert(driver.statsCalls == 1);
        std::cout << "PASS held native gate never blocks IRQ/NAPI producers\n";
    }
    {
        RTWCore89 driver; std::atomic<bool> done {false};
        std::thread consumer([&] { while (!done) { driver.loop.fire(); std::this_thread::yield(); } });
        std::vector<std::thread> producers;
        for (unsigned n = 0; n < 4; ++n) producers.emplace_back([&] {
            for (unsigned i = 0; i < 5000; ++i) queueReceiveFrame(&driver.linuxContext, packet());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        driver.stopNativeRx();
        for (auto &p : producers) p.join();
        done = true; consumer.join();
        driver.stopNativeRx();
        std::cout << "PASS concurrent producers, consumer and stop\n";
    }
    assert(allocated == freed);
    std::cout << "ALL_NATIVE_RX_OWNERSHIP_AND_CONCURRENCY_TESTS_PASSED " << freed << " buffers\n";
}
