#include "bits.h"

/* the number of bits that are 1, with the CPU's own instruction where there is one */
int64_t bits_set(uint64_t v) {
#if defined(__x86_64__) && defined(__POPCNT__)
    uint64_t n;
    __asm__("popcnt %1, %0" : "=r"(n) : "r"(v));
    return (int64_t)n;
#else
    return __builtin_popcountll(v);
#endif
}

uint64_t bits_mix(uint64_t a, uint64_t b) {
    return (a ^ b) * 0x9e3779b97f4a7c15ULL;
}
