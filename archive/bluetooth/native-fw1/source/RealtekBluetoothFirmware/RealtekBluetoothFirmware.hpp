//
//  RealtekBluetoothFirmware.hpp
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#ifndef RealtekBluetoothFirmware_H
#define RealtekBluetoothFirmware_H

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/usb/USB.h>
#include <libkern/OSKextLib.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>

#include "BtRtl.hpp"

class RealtekBluetoothFirmware : public IOService
{
    OSDeclareDefaultStructors(RealtekBluetoothFirmware)

public:
    bool init(OSDictionary *dictionary = NULL) override;
    void free() override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;

    void cleanUp();
    void publishReg(bool isSucceed, const char *fwName);

private:
    BtRtl           *m_pBTRtl;
    IOUSBHostDevice *m_pDevice;
};

#endif
