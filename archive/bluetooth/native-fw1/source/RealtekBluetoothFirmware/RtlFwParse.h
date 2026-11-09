//
//  RtlFwParse.h
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#ifndef RtlFwParse_h
#define RtlFwParse_h

#include <stdint.h>
#include <string.h>

#define RTL_LMP_8723A 0x1200
#define RTL_LMP_8723B 0x8723
#define RTL_LMP_8821A 0x8821
#define RTL_LMP_8761A 0x8761
#define RTL_LMP_8703B 0x8703
#define RTL_LMP_8822B 0x8822
#define RTL_LMP_8852A 0x8852
#define RTL_LMP_8851B 0x8851
#define RTL_LMP_8922A 0x8922

#define RTL_SEC_SNIPPETS         0x01
#define RTL_SEC_DUMMY_HEADER     0x02
#define RTL_SEC_SECURITY_HEADER  0x03

typedef struct {
    uint32_t       opcode;
    uint8_t        prio;
    uint32_t       len;
    const uint8_t *data;
} RtlSubsecRef;

static inline uint16_t rtlLe16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rtlLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t rtlLmpForProjectId(int id)
{
    static const struct { uint16_t lmp; int id; } table[] = {
        { RTL_LMP_8723A, 0 },  { RTL_LMP_8723B, 1 },  { RTL_LMP_8821A, 2 },
        { RTL_LMP_8761A, 3 },  { RTL_LMP_8703B, 7 },  { RTL_LMP_8822B, 8 },
        { RTL_LMP_8723B, 9 },  { RTL_LMP_8821A, 10 }, { RTL_LMP_8822B, 13 },
        { RTL_LMP_8761A, 14 }, { RTL_LMP_8852A, 18 }, { RTL_LMP_8852A, 20 },
        { RTL_LMP_8852A, 25 }, { RTL_LMP_8851B, 36 }, { RTL_LMP_8922A, 44 },
        { RTL_LMP_8852A, 47 }, { RTL_LMP_8761A, 51 },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].id == id)
            return table[i].lmp;
    return 0;
}

static inline int rtlFindProjectId(const uint8_t *fw, uint32_t fwLen)
{
    static const uint8_t ext_sig[4] = { 0x51, 0x04, 0xfd, 0x77 };
    if (fwLen <= 8)
        return -1;
    const uint8_t *fwptr = fw + fwLen - sizeof(ext_sig);
    if (memcmp(fwptr, ext_sig, sizeof(ext_sig)) != 0)
        return -2;
    const uint8_t *low = fw + 17;
    while (fwptr >= low) {
        uint8_t opcode = *--fwptr;
        uint8_t length = *--fwptr;
        uint8_t data   = *--fwptr;
        if (opcode == 0xff)
            break;
        if (length == 0)
            return -3;
        if (opcode == 0 && length == 1)
            return data;
        fwptr -= length;
    }
    return -4;
}

static inline uint32_t rtlParseFirmwareV2(const uint8_t *fw, uint32_t fwLen,
                                          uint8_t romVersion, uint8_t keyId,
                                          RtlSubsecRef *scratch, uint32_t scratchCap,
                                          uint8_t *out, uint32_t outCap)
{
    const uint8_t *cur = fw;
    uint32_t left = (fwLen > 7) ? fwLen - 7 : 0;

    if (left < 20)
        return 0;
    uint32_t num_sections = rtlLe32(cur + 16);
    cur  += 20;
    left -= 20;

    int count = 0;
    uint32_t total = 0;

    for (uint32_t i = 0; i < num_sections; i++) {
        if (left < 8)
            break;
        uint32_t opcode      = rtlLe32(cur);
        uint32_t section_len = rtlLe32(cur + 4);
        cur += 8; left -= 8;
        if (left < section_len)
            break;
        const uint8_t *sdata = cur;
        cur += section_len; left -= section_len;

        int wanted = (opcode == RTL_SEC_SNIPPETS) ||
                     (opcode == RTL_SEC_DUMMY_HEADER) ||
                     (opcode == RTL_SEC_SECURITY_HEADER && keyId != 0);
        if (!wanted)
            continue;

        const uint8_t *scur = sdata;
        uint32_t sleft = section_len;
        if (sleft < 4)
            continue;
        uint16_t num_subsecs = rtlLe16(scur);
        scur += 4; sleft -= 4;

        for (uint16_t k = 0; k < num_subsecs; k++) {
            if (sleft < 8)
                break;
            uint8_t  eco    = scur[0];
            uint8_t  prio   = scur[1];
            uint8_t  key_id = scur[2];
            uint32_t sublen = rtlLe32(scur + 4);
            scur += 8; sleft -= 8;
            if (sleft < sublen)
                break;
            const uint8_t *ptr = scur;
            scur += sublen; sleft -= sublen;

            if (eco != (uint8_t)(romVersion + 1))
                continue;
            if (opcode == RTL_SEC_SECURITY_HEADER && key_id != keyId)
                continue;
            if ((uint32_t)count >= scratchCap)
                break;

            int j = 0;
            while (j < count && scratch[j].prio < prio)
                j++;
            for (int m = count; m > j; m--)
                scratch[m] = scratch[m - 1];
            scratch[j].opcode = opcode;
            scratch[j].prio   = prio;
            scratch[j].len    = sublen;
            scratch[j].data   = ptr;
            count++;
            total += sublen;
        }
    }

    if (total == 0)
        return 0;
    if (out == NULL || outCap < total)
        return total;

    uint32_t off = 0;
    for (int j = 0; j < count; j++) {
        memcpy(out + off, scratch[j].data, scratch[j].len);
        off += scratch[j].len;
    }
    return total;
}

static inline uint32_t rtlParseFirmwareV1(const uint8_t *fw, uint32_t fwLen,
                                          uint8_t romVersion,
                                          uint8_t *out, uint32_t outCap)
{
    if (fwLen < 14)
        return 0;
    uint16_t num_patches = rtlLe16(fw + 12);
    uint32_t meta = 14 + (uint32_t)num_patches * 8;
    if (fwLen < meta)
        return 0;

    const uint8_t *chip_id_base = fw + 14;
    const uint8_t *len_base = chip_id_base + 2u * num_patches;
    const uint8_t *off_base = len_base + 2u * num_patches;

    uint32_t patch_offset = 0, patch_length = 0;
    for (uint16_t i = 0; i < num_patches; i++) {
        uint16_t chip_id = rtlLe16(chip_id_base + i * 2);
        if (chip_id == (uint16_t)(romVersion + 1)) {
            patch_length = rtlLe16(len_base + i * 2);
            patch_offset = rtlLe32(off_base + i * 4);
            break;
        }
    }

    if (patch_offset == 0 || patch_length < 4)
        return 0;
    if ((uint64_t)patch_offset + patch_length > fwLen)
        return 0;
    if (out == NULL || outCap < patch_length)
        return patch_length;

    memcpy(out, fw + patch_offset, patch_length - 4);
    memcpy(out + patch_length - 4, fw + 8, 4);   /* append fw_version */
    return patch_length;
}

static inline uint32_t rtlFragmentCount(uint32_t len)
{
    return len / 252 + 1;
}

#endif /* RtlFwParse_h */
