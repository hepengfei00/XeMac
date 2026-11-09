//
//  BtRtl.hpp
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#ifndef BtRtl_hpp
#define BtRtl_hpp

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>

#include "USBDeviceController.hpp"
#include "Hci.h"

#define RTL_FRAG_LEN            252

#define RTL_EPATCH_SIGNATURE    "Realtech"
#define RTL_EPATCH_SIGNATURE_V2 "RTBTCore"

#define RTL_ROM_LMP_8723A       0x1200
#define RTL_ROM_LMP_8723B       0x8723
#define RTL_ROM_LMP_8821A       0x8821
#define RTL_ROM_LMP_8761A       0x8761
#define RTL_ROM_LMP_8703B       0x8703
#define RTL_ROM_LMP_8822B       0x8822
#define RTL_ROM_LMP_8852A       0x8852
#define RTL_ROM_LMP_8851B       0x8851
#define RTL_ROM_LMP_8922A       0x8922

#define RTL_OP_READ_ROM_VERSION 0xfc6d
#define RTL_OP_READ_REG16       0xfc61
#define RTL_OP_DOWNLOAD         0xfc20

#define RTL_PATCH_SNIPPETS         0x01
#define RTL_PATCH_DUMMY_HEADER     0x02
#define RTL_PATCH_SECURITY_HEADER  0x03

typedef struct __attribute__((packed)) {
    uint8_t  index;
    uint8_t  data[RTL_FRAG_LEN];
} RtlDownloadCmd;

typedef struct __attribute__((packed)) {
    uint8_t  param[5];
} RtlVendorCmd;

struct RtlIcInfo {
    uint16_t    lmp_subver;
    uint16_t    hci_rev;
    uint8_t     hci_ver;
    bool        config_needed;
    bool        has_rom_version;
    const char *fw_name;
    const char *cfg_name;
    const char *hw_info;
};

class BtRtl : public OSObject {
    OSDeclareDefaultStructors(BtRtl)

public:
    bool initWithDevice(IOService *client, IOUSBHostDevice *dev);
    void free() override;
    bool setup();
    bool getFirmwareName(char *fwname, size_t len);

private:
    bool hciCmd(uint16_t opcode, const void *param, uint8_t plen,
                const uint8_t **outParams, uint32_t *outParamLen, int timeout);

    bool readLocalVersion(HciRpReadLocalVersion *out);
    bool readRomVersion(uint8_t *version);
    bool readReg16(const RtlVendorCmd *cmd, uint8_t out[2]);

    const RtlIcInfo *matchIc(uint16_t lmp_subver, uint16_t hci_rev, uint8_t hci_ver);

    OSData *parseFirmware(uint8_t *fwData, uint32_t fwLen);
    bool downloadFirmware(const uint8_t *data, uint32_t fwLen);

    OSData *requestFirmwareData(const char *fwName, bool noWarn = false);
    OSData *firmwareConvertion(OSData *originalFirmware);

    USBDeviceController *m_pUSBDeviceController;
    const RtlIcInfo     *m_icInfo;
    uint8_t              m_romVersion;
    uint8_t              m_keyId;
    char                 m_fwName[48];

    uint8_t              m_cmdBuf[260];
    uint8_t              m_evtBuf[260];
};

#endif /* BtRtl_hpp */
