#include <stdint.h>

static int g_plugin_bias = 12;

int plugin_init_val(void) {
    return g_plugin_bias;
}

int plugin_compute(int a, int b) {
    /* For (7, 5): 7*5 + 12 = 47 */
    return (a * b) + g_plugin_bias;
}

const char *plugin_name(void) {
    return "newminhh_plugin_s03";
}
