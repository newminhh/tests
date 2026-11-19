#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_counter = 10;

__attribute__((noinline))
static int switch_step(int op, int a, int b) {
    int r = 0;
    switch (op & 0xf) {
    case 0:  r = a + b + 1; break;
    case 1:  r = a - b + 2; break;
    case 2:  r = (a * 3) ^ b; break;
    case 3:  r = (a + b) * 5; break;
    case 4:  r = a ^ (b * 7); break;
    case 5:  r = a + (b << 1) + 3; break;
    case 6:  r = (a << 2) - b; break;
    case 7:  r = (a + 11) ^ (b + 13); break;
    case 8:  r = a * 2 + b * 3; break;
    case 9:  r = (a - 5) * (b + 1); break;
    case 10: r = (a ^ 0x55) + b; break;
    case 11: r = a + (b ^ 0xaa); break;
    case 12: r = (a + b) * 7 + 1; break;
    case 13: r = (a * 11) - b; break;
    case 14: r = (a ^ b) + 17; break;
    case 15: r = a + b + 19; break;
    default: r = a ^ b; break;
    }
    return r;
}

int basic_add_mul(int a, int b, int c) {
    g_counter += 1;
    int acc = switch_step(c, a, b);
    for (int i = 0; i < 4; i++) {
        acc = switch_step(c + i, acc, b + i);
    }
    return acc;
}

int basic_format_msg(char *buf, int buf_len, int x) {
    if (!buf || buf_len <= 0) return -1;
    char *tmp = (char *)malloc(64);
    if (!tmp) return -1;
    int n = snprintf(tmp, 64, "BASIC:%d:%d", x, g_counter);
    if (n < 0 || n >= buf_len) {
        free(tmp);
        return -1;
    }
    memcpy(buf, tmp, (size_t)n + 1);
    free(tmp);
    return n;
}

int basic_get_counter(void) {
    return g_counter;
}
