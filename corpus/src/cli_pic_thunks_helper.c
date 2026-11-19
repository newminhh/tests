#include <stdint.h>

uint32_t pic_global_arr[6] = {10, 20, 30, 40, 50, 60};
__thread uint32_t pic_tls_counter = 0x100u;

__attribute__((noinline))
uint32_t pic_helper_fn(uint32_t x, uint32_t idx) {
    pic_tls_counter += x + 3u;
    return pic_global_arr[idx % 6] + x * 5u;
}
