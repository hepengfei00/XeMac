#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
struct OSObject {};
using IOReturn = int;
constexpr int kIOReturnSuccess = 0;
struct Mbuf { Mbuf *next; unsigned id; };
using mbuf_t = Mbuf *;
struct mbuf_list { mbuf_t head = nullptr, tail = nullptr; unsigned count = 0; };
mbuf_t ml_dequeue(mbuf_list *p) {
    auto *m = p->head;
    if (m) { p->head = m->next; if (!p->head) p->tail = nullptr; --p->count; m->next = nullptr; }
    return m;
}
void panic(const char *, ...) { std::abort(); }
void XYLog(const char *, ...) {}
struct IONetworkInterface { static constexpr unsigned kInputOptionQueuePacket = 1; };
struct Interface {
    std::vector<unsigned> ids;
    unsigned flushes = 0;
    void inputPacket(mbuf_t m, unsigned, unsigned) {
        // Ownership has transferred. Reusing/freeing this packet must never
        // make the caller lose, dereference or leak the rest of its list.
        assert(m->next == nullptr); ids.push_back(m->id); delete m;
    }
    void flushInputQueue() { ++flushes; }
};
struct NetStat { unsigned inputPackets = 0; };
struct _ifnet { Interface *iface; NetStat *netStat; };
#include "interface-input-under-test.inc"
int main() {
    Interface interface; NetStat stats; _ifnet net{&interface, &stats}; mbuf_list list;
    for (unsigned i = 0; i < 512; ++i) {
        auto *m = new Mbuf{nullptr, i};
        if (list.tail) list.tail->next = m; else list.head = m;
        list.tail = m; ++list.count;
    }
    _if_input(nullptr, &net, &list, nullptr, nullptr);
    assert(list.count == 0 && !list.head && !list.tail);
    assert(interface.ids.size() == 512 && stats.inputPackets == 512 && interface.flushes == 1);
    for (unsigned i = 0; i < 512; ++i) assert(interface.ids[i] == i);
    _if_input(nullptr, &net, &list, nullptr, nullptr);
    assert(interface.flushes == 1);
    std::cout << "PASS production interface batch: 512 ordered ownership transfers, one flush, empty-list handling\n";
}
