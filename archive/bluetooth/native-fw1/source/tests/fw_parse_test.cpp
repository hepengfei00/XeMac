//
//  fw_parse_test.cpp
//  RealtekBluetoothFirmware
//
//  Created by theVakhovskeIsTaken on 2026/8/9.
//  Copyright © 2026 thegwchr. All rights reserved.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#include "../RealtekBluetoothFirmware/RtlFwParse.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok   - " __VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL - " __VA_ARGS__); printf("\n"); g_fail++; } \
} while (0)

static std::vector<uint8_t> readFile(const char *path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(n);
    if (fread(data.data(), 1, n, f) != (size_t)n) { fprintf(stderr, "read error\n"); exit(2); }
    fclose(f);
    return data;
}

static void validateV2(const char *name, const std::vector<uint8_t> &fw)
{
    const uint8_t *p = fw.data();
    uint32_t len = (uint32_t)fw.size();

    int pid = rtlFindProjectId(p, len);
    CHECK(pid >= 0, "%s: project id found (%d)", name, pid);
    uint16_t lmp = rtlLmpForProjectId(pid);
    CHECK(lmp != 0, "%s: project %d maps to lmp 0x%04x", name, pid, lmp);

    std::vector<RtlSubsecRef> scratch(512);
    std::vector<uint8_t> out(len);
    bool anyPayload = false, allInRange = true;
    uint32_t bestRom = 0, bestLen = 0;
    for (uint8_t rom = 0; rom < 16; rom++) {
        uint32_t got = rtlParseFirmwareV2(p, len, rom, 0, scratch.data(), 512, out.data(), len);
        if (got > len) allInRange = false;
        if (got > 0 && got <= len) { anyPayload = true; if (got > bestLen) { bestLen = got; bestRom = rom; } }
    }
    CHECK(allInRange, "%s: no rom_version needs more than fwLen", name);
    CHECK(anyPayload, "%s: a rom_version yields a payload (rom %u, %u bytes)", name, bestRom, bestLen);

    if (strstr(name, "8822cu")) {
        CHECK(pid == 13 && lmp == 0x8822, "%s: identified as RTL8822C", name);
    }
}

static void validateV1(const char *name, const std::vector<uint8_t> &fw)
{
    const uint8_t *p = fw.data();
    uint32_t len = (uint32_t)fw.size();

    int pid = rtlFindProjectId(p, len);
    CHECK(pid >= 0, "%s: project id found (%d)", name, pid);
    uint16_t lmp = rtlLmpForProjectId(pid);
    CHECK(lmp != 0, "%s: project %d maps to lmp 0x%04x", name, pid, lmp);

    std::vector<uint8_t> out(len);
    bool anyPayload = false, allInRange = true;
    uint32_t bestRom = 0, bestLen = 0;
    for (uint8_t rom = 0; rom < 16; rom++) {
        uint32_t got = rtlParseFirmwareV1(p, len, rom, out.data(), len);
        if (got > len) allInRange = false;
        if (got > 0 && got <= len) { anyPayload = true; if (got > bestLen) { bestLen = got; bestRom = rom; } }
    }
    CHECK(allInRange, "%s: no rom_version needs more than fwLen", name);
    CHECK(anyPayload, "%s: a rom_version yields a payload (rom %u, %u bytes)", name, bestRom, bestLen);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "fw";

    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "cannot open dir %s\n", dir); return 2; }
    std::vector<std::string> files;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        std::string n = ent->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bin") == 0)
            files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    int v2count = 0, v1count = 0;
    for (const auto &n : files) {
        std::string path = std::string(dir) + "/" + n;
        std::vector<uint8_t> fw = readFile(path.c_str());
        if (fw.size() > 8 && memcmp(fw.data(), "RTBTCore", 8) == 0) {
            printf("v2 firmware: %s (%zu bytes)\n", n.c_str(), fw.size());
            validateV2(n.c_str(), fw);
            v2count++;
        } else if (fw.size() > 8 && memcmp(fw.data(), "Realtech", 8) == 0) {
            printf("v1 firmware: %s (%zu bytes)\n", n.c_str(), fw.size());
            validateV1(n.c_str(), fw);
            v1count++;
        }
    }
    CHECK(v2count >= 6, "found %d v2 firmware files (expected >= 6)", v2count);
    CHECK(v1count >= 8, "found %d v1 firmware files (expected >= 8)", v1count);

    {
        uint32_t L = 37268;
        uint32_t frags = rtlFragmentCount(L);
        CHECK(frags == L / 252 + 1, "fragment count for %u bytes == %u", L, frags);
        int j = 0; uint8_t lastIndex = 0;
        for (uint32_t i = 0; i < frags; i++) {
            uint8_t index = (uint8_t)(j++);
            if (index == 0x7f) j = 1;
            if (i == frags - 1) index |= 0x80;
            lastIndex = index;
        }
        CHECK((lastIndex & 0x80) != 0, "last fragment sets end flag (0x%02x)", lastIndex);
    }

    {
        std::vector<RtlSubsecRef> scratch(512);
        std::vector<uint8_t> out(16);
        uint8_t junk4[4] = { 0 };
        uint32_t r1 = rtlParseFirmwareV2(junk4, 4, 3, 0, scratch.data(), 512, out.data(), 16);
        CHECK(r1 == 0, "truncated firmware returns 0");
        std::vector<uint8_t> junk(64, 0xAB);
        int jr = rtlFindProjectId(junk.data(), junk.size());
        CHECK(jr < 0, "garbage firmware: project id not found (%d)", jr);
    }

    printf("\n%s\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
