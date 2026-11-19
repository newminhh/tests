#include <stdint.h>
#include <stdio.h>

static volatile uint32_t g_sink = 0;

__attribute__((noinline))
static uint32_t branch_work(uint32_t base, uint32_t mul) {
    uint32_t v = (base ^ 0x5a5au) * mul;
    g_sink ^= v;
    return v + 7u;
}

__attribute__((noinline))
static uint32_t dispatch_switch(int idx, uint32_t seed) {
    uint32_t r = seed;
    switch (idx) {
    case 0:  r = branch_work(r + 11u, 3u); break;
    case 1:  r = branch_work(r + 23u, 5u) ^ 0x111u; break;
    case 2:  r = branch_work(r + 37u, 7u) + 19u; break;
    case 3:  r = branch_work(r + 41u, 11u) ^ 0x222u; break;
    case 4:  r = branch_work(r + 53u, 13u) + 29u; break;
    case 5:  r = branch_work(r + 61u, 17u) ^ 0x333u; break;
    case 6:  r = branch_work(r + 73u, 19u) + 43u; break;
    case 7:  r = branch_work(r + 89u, 23u) ^ 0x444u; break;
    case 8:  r = branch_work(r + 97u, 29u) + 59u; break;
    case 9:  r = branch_work(r + 101u, 31u) ^ 0x555u; break;
    case 10: r = branch_work(r + 113u, 37u) + 71u; break;
    case 11: r = branch_work(r + 127u, 41u) ^ 0x666u; break;
    case 12: r = branch_work(r + 131u, 43u) + 83u; break;
    case 13: r = branch_work(r + 149u, 47u) ^ 0x777u; break;
    case 14: r = branch_work(r + 157u, 53u) + 97u; break;
    case 15: r = branch_work(r + 167u, 59u) ^ 0x888u; break;
    case 16: r = branch_work(r + 179u, 61u) + 109u; break;
    case 17: r = branch_work(r + 191u, 67u) ^ 0x999u; break;
    case 18: r = branch_work(r + 199u, 71u) + 127u; break;
    case 19: r = branch_work(r + 211u, 73u) ^ 0xaaau; break;
    case 20: r = branch_work(r + 223u, 79u) + 139u; break;
    case 21: r = branch_work(r + 233u, 83u) ^ 0xbbbu; break;
    case 22: r = branch_work(r + 241u, 89u) + 151u; break;
    case 23: r = branch_work(r + 251u, 97u) ^ 0xcccu; break;
    default: r = (r * 131u) ^ (uint32_t)(idx + 0x7f); break;
    }
    return r;
}

int main(void) {
    uint32_t csum = 0x12345678u;
    for (int i = 0; i <= 25; i++) {
        csum = dispatch_switch(i, csum);
    }
    printf("SWITCH_OK csum=0x%08x\n", csum);
    return 0;
}
