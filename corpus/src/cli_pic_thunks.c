#include <stdint.h>
#include <stdio.h>

extern uint32_t pic_global_arr[6];
extern __thread uint32_t pic_tls_counter;
uint32_t pic_helper_fn(uint32_t x, uint32_t idx);

static __thread uint32_t local_tls_acc = 42u;
static uint32_t (*volatile g_fn_ptr)(uint32_t, uint32_t) = pic_helper_fn;

__attribute__((noinline))
static uint32_t local_pic_compute(uint32_t seed) {
    uint32_t v = seed;
    for (uint32_t i = 0; i < 6; i++) {
        v += g_fn_ptr(pic_global_arr[i], i);
        local_tls_acc ^= (v + i);
    }
    return v;
}

int main(void) {
    uint32_t val = local_pic_compute(7u);
    uint32_t tls = pic_tls_counter ^ local_tls_acc;
    printf("PIC_OK val=%u tls=0x%08x\n", val, tls);
    return 0;
}
