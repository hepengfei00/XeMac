//
//  BtRtl.cpp
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#include "Log.h"
#include "BtRtl.hpp"
#include "FwData.h"
#include "RtlFwParse.h"

#define super OSObject
OSDefineMetaClassAndStructors(BtRtl, OSObject)

static const RtlIcInfo kIcTable[] = {
    /* v2 (RTBTCore) firmware */
    { RTL_ROM_LMP_8822B, 0x000c, 0x0a, false, true, "rtl8822cu_fw.bin",    NULL,                   "rtl8822cu" },
    { RTL_ROM_LMP_8852A, 0x000b, 0x0b, false, true, "rtl8852bu_fw.bin",    NULL,                   "rtl8852bu" },
    { RTL_ROM_LMP_8852A, 0x000c, 0x0c, false, true, "rtl8852cu_fw_v2.bin", NULL,                   "rtl8852cu" },
    { RTL_ROM_LMP_8852A, 0x0087, 0x0c, false, true, "rtl8852btu_fw.bin",   NULL,                   "rtl8852btu" },
    { RTL_ROM_LMP_8851B, 0x000b, 0x0c, false, true, "rtl8851bu_fw.bin",    NULL,                   "rtl8851bu" },
    { RTL_ROM_LMP_8922A, 0x000a, 0x0c, false, true, "rtl8922au_fw.bin",    NULL,                   "rtl8922au" },

    /* v1 (Realtech) firmware */
    { RTL_ROM_LMP_8723B, 0x000b, 0x06, false, true, "rtl8723b_fw.bin",     NULL,                   "rtl8723bu" },
    { RTL_ROM_LMP_8821A, 0x000a, 0x06, false, true, "rtl8821a_fw.bin",     NULL,                   "rtl8821au" },
    { RTL_ROM_LMP_8821A, 0x000c, 0x08, false, true, "rtl8821c_fw.bin",     "rtl8821c_config.bin",  "rtl8821cu" },
    { RTL_ROM_LMP_8761A, 0x000a, 0x06, false, true, "rtl8761a_fw.bin",     NULL,                   "rtl8761au" },
    { RTL_ROM_LMP_8761A, 0x000b, 0x0a, false, true, "rtl8761bu_fw.bin",    "rtl8761bu_config.bin", "rtl8761bu" },
    { RTL_ROM_LMP_8761A, 0x000e, 0x00, false, true, "rtl8761cu_fw.bin",    "rtl8761cu_config.bin", "rtl8761cu" },
    { RTL_ROM_LMP_8822B, 0x000b, 0x07, true,  true, "rtl8822b_fw.bin",     "rtl8822b_config.bin",  "rtl8822bu" },
    { RTL_ROM_LMP_8852A, 0x000a, 0x0b, false, true, "rtl8852au_fw.bin",    NULL,                   "rtl8852au" },
};

bool BtRtl::
initWithDevice(IOService *client, IOUSBHostDevice *dev)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (!super::init())
        return false;

    m_icInfo = NULL;
    m_romVersion = 0;
    m_keyId = 0;
    m_fwName[0] = '\0';

    m_pUSBDeviceController = new USBDeviceController();
    if (!m_pUSBDeviceController->init(client, dev))
        return false;
    if (!m_pUSBDeviceController->initConfiguration())
        return false;
    if (!m_pUSBDeviceController->findInterface())
        return false;
    if (!m_pUSBDeviceController->findPipes())
        return false;
    return true;
}

void BtRtl::
free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    OSSafeReleaseNULL(m_pUSBDeviceController);
    super::free();
}

bool BtRtl::
getFirmwareName(char *fwname, size_t len)
{
    if (!fwname || len == 0)
        return false;
    strlcpy(fwname, m_fwName[0] ? m_fwName : "rtl_unknown", len);
    return true;
}

bool BtRtl::
hciCmd(uint16_t opcode, const void *param, uint8_t plen,
       const uint8_t **outParams, uint32_t *outParamLen, int timeout)
{
    HciCommandHdr *cmd = (HciCommandHdr *)m_cmdBuf;
    cmd->opcode = OSSwapHostToLittleInt16(opcode);
    cmd->len = plen;
    if (plen && param)
        memcpy(cmd->data, param, plen);

    IOReturn ret = m_pUSBDeviceController->sendHCIRequest(cmd, timeout);
    if (ret != kIOReturnSuccess) {
        XYLog("hciCmd(0x%04x) send failed: %s\n", opcode,
              m_pUSBDeviceController->stringFromReturn(ret));
        return false;
    }

    for (int i = 0; i < 16; i++) {
        uint32_t evtLen = 0;
        ret = m_pUSBDeviceController->interruptPipeRead(m_evtBuf, sizeof(m_evtBuf),
                                                        &evtLen, timeout);
        if (ret != kIOReturnSuccess) {
            XYLog("hciCmd(0x%04x) event read failed: %s\n", opcode,
                  m_pUSBDeviceController->stringFromReturn(ret));
            return false;
        }
        if (evtLen < 2)
            continue;
        if (m_evtBuf[0] != HCI_EV_CMD_COMPLETE)
            continue;
        if (evtLen < 5)
            continue;
        if (OSReadLittleInt16(m_evtBuf, 3) != opcode)
            continue;

        uint8_t hlen = m_evtBuf[1];
        uint32_t rlen = (hlen >= 3) ? (uint32_t)(hlen - 3) : 0;
        if (5 + rlen > evtLen)
            rlen = evtLen - 5;
        if (outParams)   *outParams = m_evtBuf + 5;
        if (outParamLen) *outParamLen = rlen;
        return true;
    }
    XYLog("hciCmd(0x%04x) no command-complete event\n", opcode);
    return false;
}

bool BtRtl::
readLocalVersion(HciRpReadLocalVersion *out)
{
    const uint8_t *p = NULL;
    uint32_t plen = 0;
    if (!hciCmd(HCI_OP_READ_LOCAL_VERSION, NULL, 0, &p, &plen, HCI_INIT_TIMEOUT))
        return false;
    if (plen < 9) {
        XYLog("read local version: short event (%u)\n", plen);
        return false;
    }
    if (p[0] != 0) {
        XYLog("read local version: status 0x%02x\n", p[0]);
        return false;
    }
    out->status       = p[0];
    out->hci_ver      = p[1];
    out->hci_rev      = OSReadLittleInt16(p, 2);
    out->lmp_ver      = p[4];
    out->manufacturer = OSReadLittleInt16(p, 5);
    out->lmp_subver   = OSReadLittleInt16(p, 7);
    return true;
}

bool BtRtl::
readRomVersion(uint8_t *version)
{
    const uint8_t *p = NULL;
    uint32_t plen = 0;
    if (!hciCmd(RTL_OP_READ_ROM_VERSION, NULL, 0, &p, &plen, HCI_INIT_TIMEOUT))
        return false;
    if (plen < 2 || p[0] != 0) {
        XYLog("read rom version failed (len %u status 0x%02x)\n", plen, plen ? p[0] : 0xff);
        return false;
    }
    *version = p[1];
    return true;
}

bool BtRtl::
readReg16(const RtlVendorCmd *cmd, uint8_t out[2])
{
    const uint8_t *p = NULL;
    uint32_t plen = 0;
    if (!hciCmd(RTL_OP_READ_REG16, cmd->param, sizeof(cmd->param), &p, &plen, HCI_INIT_TIMEOUT))
        return false;
    if (plen < 3 || p[0] != 0)
        return false;
    out[0] = p[1];
    out[1] = p[2];
    return true;
}

const RtlIcInfo *BtRtl::
matchIc(uint16_t lmp_subver, uint16_t hci_rev, uint8_t hci_ver)
{
    for (size_t i = 0; i < sizeof(kIcTable) / sizeof(kIcTable[0]); i++) {
        const RtlIcInfo *ic = &kIcTable[i];
        if (ic->lmp_subver == lmp_subver && ic->hci_rev == hci_rev && ic->hci_ver == hci_ver)
            return ic;
    }
    return NULL;
}

#define RTL_MAX_SUBSECS 512

OSData *BtRtl::
parseFirmware(uint8_t *fwData, uint32_t fwLen)
{
    if (fwLen <= 8)
        return NULL;

    bool isV2 = memcmp(fwData, RTL_EPATCH_SIGNATURE_V2, 8) == 0;
    bool isV1 = memcmp(fwData, RTL_EPATCH_SIGNATURE, 8) == 0;
    if (!isV1 && !isV2) {
        XYLog("bad epatch signature\n");
        return NULL;
    }

    int project_id = rtlFindProjectId(fwData, fwLen);
    if (project_id < 0) {
        XYLog("failed to find project-id instruction (%d)\n", project_id);
        return NULL;
    }
    uint16_t lmp = rtlLmpForProjectId(project_id);
    if (lmp == 0) {
        XYLog("unknown project id %d\n", project_id);
        return NULL;
    }
    if (m_icInfo && lmp != m_icInfo->lmp_subver) {
        XYLog("firmware is for %04x but this is a %04x\n", lmp, m_icInfo->lmp_subver);
        return NULL;
    }
    XYLog("firmware project id %d (lmp %04x, %s)\n", project_id, lmp, isV2 ? "v2" : "v1");

    uint8_t *out = (uint8_t *)IOMalloc(fwLen);
    if (!out)
        return NULL;
    OSData *result = NULL;
    uint32_t len = 0;

    if (isV2) {
        size_t scratchBytes = sizeof(RtlSubsecRef) * RTL_MAX_SUBSECS;
        RtlSubsecRef *scratch = (RtlSubsecRef *)IOMalloc(scratchBytes);
        if (scratch) {
            len = rtlParseFirmwareV2(fwData, fwLen, m_romVersion, m_keyId,
                                     scratch, RTL_MAX_SUBSECS, out, fwLen);
            IOFree(scratch, scratchBytes);
        }
    } else {
        len = rtlParseFirmwareV1(fwData, fwLen, m_romVersion, out, fwLen);
    }

    if (len > 0 && len <= fwLen) {
        XYLog("parsed firmware payload: %u bytes (rom_version %u key_id %u)\n",
              len, m_romVersion, m_keyId);
        result = OSData::withBytes(out, len);
    } else {
        XYLog("firmware parse produced no usable payload (%u)\n", len);
    }

    IOFree(out, fwLen);
    return result;
}

bool BtRtl::
downloadFirmware(const uint8_t *data, uint32_t fwLen)
{
    RtlDownloadCmd dl;
    uint32_t frag_num = fwLen / RTL_FRAG_LEN + 1;
    uint32_t frag_len = RTL_FRAG_LEN;
    int j = 0;
    const uint8_t *p = data;

    XYLog("downloading firmware: %u bytes in %u fragments\n", fwLen, frag_num);

    for (uint32_t i = 0; i < frag_num; i++) {
        dl.index = (uint8_t)(j++);
        if (dl.index == 0x7f)
            j = 1;
        if (i == frag_num - 1) {
            dl.index |= 0x80;
            frag_len = fwLen % RTL_FRAG_LEN;
        }
        if (frag_len)
            memcpy(dl.data, p, frag_len);

        const uint8_t *params = NULL;
        uint32_t plen = 0;
        if (!hciCmd(RTL_OP_DOWNLOAD, &dl, (uint8_t)(frag_len + 1),
                    &params, &plen, HCI_INIT_TIMEOUT)) {
            XYLog("download fragment %u/%u failed\n", i, frag_num);
            return false;
        }
        if (plen < 2 || params[0] != 0) {
            XYLog("download fragment %u status 0x%02x\n", i, plen ? params[0] : 0xff);
            return false;
        }
        p += RTL_FRAG_LEN;
    }

    HciRpReadLocalVersion ver;
    if (readLocalVersion(&ver))
        XYLog("post-download version: hci_rev %04x lmp_subver %04x\n",
              ver.hci_rev, ver.lmp_subver);
    return true;
}

OSData *BtRtl::
firmwareConvertion(OSData *originalFirmware)
{
    unsigned int numBytes = originalFirmware->getLength() * 4;
    unsigned int actualBytes = numBytes;
    OSData *fwData = NULL;
    unsigned char *_fwBytes = (unsigned char *)IOMalloc(numBytes);
    if (!_fwBytes)
        return NULL;
    if (!uncompressFirmware(_fwBytes, &actualBytes,
                            (unsigned char *)originalFirmware->getBytesNoCopy(),
                            originalFirmware->getLength())) {
        IOFree(_fwBytes, numBytes);
        return NULL;
    }
    fwData = OSData::withBytes(_fwBytes, actualBytes);
    IOFree(_fwBytes, numBytes);
    return fwData;
}

OSData *BtRtl::
requestFirmwareData(const char *fwName, bool noWarn)
{
    OSData *_fwData = getFWDescByName(fwName);
    if (!_fwData) {
        if (!noWarn)
            XYLog("embedded firmware %s not found\n", fwName);
        return NULL;
    }
    OSData *fwData = firmwareConvertion(_fwData);
    OSSafeReleaseNULL(_fwData);
    if (!fwData) {
        XYLog("firmware %s uncompress failed\n", fwName);
        return NULL;
    }
    XYLog("loaded firmware %s (%u bytes)\n", fwName, fwData->getLength());
    return fwData;
}

bool BtRtl::
setup()
{
    HciRpReadLocalVersion ver;
    if (!readLocalVersion(&ver))
        return false;
    XYLog("hci_ver=%02x hci_rev=%04x lmp_ver=%02x lmp_subver=%04x\n",
          ver.hci_ver, ver.hci_rev, ver.lmp_ver, ver.lmp_subver);

    m_icInfo = matchIc(ver.lmp_subver, ver.hci_rev, ver.hci_ver);
    if (!m_icInfo) {
        XYLog("unsupported controller (lmp_subver %04x hci_rev %04x hci_ver %02x)\n",
              ver.lmp_subver, ver.hci_rev, ver.hci_ver);
        return false;
    }
    strlcpy(m_fwName, m_icInfo->hw_info, sizeof(m_fwName));
    XYLog("matched %s\n", m_icInfo->hw_info);

    if (m_icInfo->has_rom_version) {
        if (!readRomVersion(&m_romVersion))
            return false;
        XYLog("rom_version %u\n", m_romVersion);
    }

    m_keyId = 0;
    RtlVendorCmd secProj = { { 0x10, 0xA4, 0xAD, 0x00, 0xb0 } };
    uint8_t reg[2];
    if (readReg16(&secProj, reg))
        m_keyId = reg[0];
    else
        XYLog("could not read key id, assuming 0\n");

    OSData *fwRaw = requestFirmwareData(m_icInfo->fw_name);
    if (!fwRaw)
        return false;

    OSData *payload = parseFirmware((uint8_t *)fwRaw->getBytesNoCopy(), fwRaw->getLength());
    OSSafeReleaseNULL(fwRaw);
    if (!payload) {
        XYLog("firmware parse failed\n");
        return false;
    }

    if (m_icInfo->cfg_name && m_keyId == 0) {
        OSData *cfg = requestFirmwareData(m_icInfo->cfg_name, !m_icInfo->config_needed);
        if (cfg) {
            unsigned int pl = payload->getLength();
            unsigned int cl = cfg->getLength();
            uint8_t *merged = (uint8_t *)IOMalloc(pl + cl);
            if (merged) {
                memcpy(merged, payload->getBytesNoCopy(), pl);
                memcpy(merged + pl, cfg->getBytesNoCopy(), cl);
                OSSafeReleaseNULL(payload);
                payload = OSData::withBytes(merged, pl + cl);
                IOFree(merged, pl + cl);
                XYLog("appended config %s (%u bytes)\n", m_icInfo->cfg_name, cl);
            }
            OSSafeReleaseNULL(cfg);
        } else if (m_icInfo->config_needed) {
            XYLog("mandatory config %s missing\n", m_icInfo->cfg_name);
            OSSafeReleaseNULL(payload);
            return false;
        }
    }

    if (!payload)
        return false;

    bool ok = downloadFirmware((const uint8_t *)payload->getBytesNoCopy(),
                               payload->getLength());
    OSSafeReleaseNULL(payload);

    if (ok)
        XYLog("firmware download complete for %s\n", m_icInfo->hw_info);
    return ok;
}
