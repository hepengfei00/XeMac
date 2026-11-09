#include "rtw/PrivateSPI.pch"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
static_assert(sizeof(IOInterruptEventSource)==104, "target IOInterruptEventSource ABI");
static_assert(sizeof(IOEventSource)==72, "target IOEventSource ABI");
static_assert(sizeof(IOWorkLoop)==72, "target IOWorkLoop ABI");
static_assert(sizeof(IOCommandGate)==80, "target IOCommandGate ABI");
void notify(IOInterruptEventSource *event) { event->interruptOccurred(nullptr, nullptr, 0); }
