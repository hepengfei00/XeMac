#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* User-space CPUID only: no MSR access, kernel driver, or hardware writes. */
int main(void) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    unsigned int max_leaf = __get_cpuid_max(0, NULL);
    char brand[49] = {0};
    if (__get_cpuid_max(0x80000000U, NULL) >= 0x80000004U) {
        for (unsigned int i = 0; i < 3; ++i) {
            unsigned int words[4];
            __cpuid_count(0x80000002U + i, 0,
                          words[0], words[1], words[2], words[3]);
            memcpy(brand + i * 16, words, sizeof(words));
        }
    }
    printf("brand=%s\nmax_basic_leaf=0x%x\n", brand, max_leaf);
    if (max_leaf >= 1) {
        __cpuid_count(1, 0, a, b, c, d);
        unsigned int family = (a >> 8) & 0xf;
        unsigned int model = (a >> 4) & 0xf;
        if (family == 6 || family == 15) model |= ((a >> 16) & 0xf) << 4;
        if (family == 15) family += (a >> 20) & 0xff;
        printf("cpuid1_eax=0x%08x\nfamily=%u\nmodel=0x%x\nstepping=%u\n",
               a, family, model, a & 0xf);
    }
    if (max_leaf >= 6) {
        __cpuid_count(6, 0, a, b, c, d);
        printf("cpuid6_eax=0x%08x\ndigital_temperature_sensor=%u\n"
               "package_thermal_management=%u\n", a, a & 1, (a >> 6) & 1);
    }
    if (max_leaf >= 0x1a) {
        __cpuid_count(0x1a, 0, a, b, c, d);
        printf("sampled_core_type=0x%x\n", a >> 24);
    }
    return 0;
}
