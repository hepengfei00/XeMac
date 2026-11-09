//
//  RealtekBluetoothFirmware.cpp
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#include "RealtekBluetoothFirmware.hpp"
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/version.h>
#include <libkern/OSTypes.h>
#include <IOKit/usb/StandardUSB.h>
#include "Hci.h"
#include "Log.h"

#define super IOService
OSDefineMetaClassAndStructors(RealtekBluetoothFirmware, IOService)

enum { kMyOffPowerState = 0, kMyOnPowerState = 1 };

#define kIOPMPowerOff 0

static IOPMPowerState myTwoStates[2] =
{
    {1, kIOPMPowerOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

bool RealtekBluetoothFirmware::init(OSDictionary *dictionary)
{
    XYLog("Driver init()\n");
    m_pBTRtl = NULL;
    m_pDevice = NULL;
    return super::init(dictionary);
}

void RealtekBluetoothFirmware::free()
{
    XYLog("Driver free()\n");
    super::free();
}

IOService *RealtekBluetoothFirmware::probe(IOService *provider, SInt32 *score)
{
    XYLog("Driver Probe()\n");
    if (!super::probe(provider, score)) {
        XYLog("super probe failed\n");
        return NULL;
    }
    IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device) {
        XYLog("provider is not a usb device\n");
        return NULL;
    }
    UInt16 vendorID  = USBToHost16(device->getDeviceDescriptor()->idVendor);
    UInt16 productID = USBToHost16(device->getDeviceDescriptor()->idProduct);
    XYLog("probe: name=%s vendorID=0x%04X productID=0x%04X\n",
          device->getName(), vendorID, productID);
    return this;
}

bool RealtekBluetoothFirmware::start(IOService *provider)
{
    XYLog("Driver Start()\n");
    char fwName[64];

    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (m_pDevice == NULL) {
        XYLog("start fail, not a usb device\n");
        return false;
    }

    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    makeUsable();

    if (!super::start(provider))
        return false;

    if (!m_pDevice->open(this)) {
        XYLog("start fail, can not open device\n");
        cleanUp();
        stop(this);
        return false;
    }

    m_pBTRtl = new BtRtl();
    if (!m_pBTRtl->initWithDevice(this, m_pDevice)) {
        XYLog("start fail, can not init device\n");
        cleanUp();
        stop(this);
        return false;
    }
    XYLog("BT init succeed\n");

    if (!m_pBTRtl->setup()) {
        XYLog("firmware setup failed\n");
        cleanUp();
        stop(this);
        return false;
    }

    m_pBTRtl->getFirmwareName(fwName, sizeof(fwName));
    publishReg(true, fwName);
    cleanUp();
    return true;
}

void RealtekBluetoothFirmware::publishReg(bool isSucceed, const char *fwName)
{
    m_pDevice->setProperty("FirmwareLoaded", isSucceed);
    if (isSucceed)
        setProperty("fw_name", OSString::withCString(fwName));
    if (version_major >= 21)
        m_pDevice->setName("Bluetooth USB Host Controller");
}

void RealtekBluetoothFirmware::cleanUp()
{
    XYLog("Clean up...\n");
    OSSafeReleaseNULL(m_pBTRtl);
    if (m_pDevice) {
        if (m_pDevice->isOpen(this))
            m_pDevice->close(this);
        m_pDevice = NULL;
    }
}

IOReturn RealtekBluetoothFirmware::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}

void RealtekBluetoothFirmware::stop(IOService *provider)
{
    XYLog("Driver Stop()\n");
    PMstop();
    super::stop(provider);
}
