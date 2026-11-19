#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int key;
    int weight;
    char tag[8];
} item_t;

static int cmp_item(const void *pa, const void *pb) {
    const item_t *a = (const item_t *)pa;
    const item_t *b = (const item_t *)pb;
    if (a->key != b->key) {
        return (a->key < b->key) ? -1 : 1;
    }
    return (a->weight > b->weight) - (a->weight < b->weight);
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_thread_sum = 0;

typedef struct {
    int id;
    uint32_t count;
} thread_arg_t;

static void *worker_fn(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    uint32_t local = 0;
    for (uint32_t i = 1; i <= ta->count; i++) {
        local += (uint32_t)ta->id * i;
    }
    pthread_mutex_lock(&g_lock);
    g_thread_sum += local;
    pthread_mutex_unlock(&g_lock);
    return (void *)(uintptr_t)(local + 1u);
}

static void atexit_handler_1(void) {
    printf("ATEXIT_1\n");
    fflush(stdout);
}

static void atexit_handler_2(void) {
    printf("ATEXIT_2\n");
    fflush(stdout);
}

int main(void) {
    item_t items[6] = {
        {42, 3, "d"},
        {10, 2, "a"},
        {99, 1, "f"},
        {10, 1, "b"},
        {25, 4, "c"},
        {77, 2, "e"},
    };

    qsort(items, 6, sizeof(item_t), cmp_item);
    for (int i = 1; i < 6; i++) {
        if (cmp_item(&items[i - 1], &items[i]) > 0) {
            fprintf(stderr, "qsort order invalid\n");
            return 1;
        }
    }

    item_t target = {25, 4, ""};
    item_t *found = (item_t *)bsearch(&target, items, 6, sizeof(item_t), cmp_item);
    if (!found || found->key != 25 || strcmp(found->tag, "c") != 0) {
        fprintf(stderr, "bsearch failed\n");
        return 1;
    }

    printf("QSORT_OK first=%d:%s last=%d:%s\n",
           items[0].key, items[0].tag, items[5].key, items[5].tag);
    printf("BSEARCH_OK key=%d tag=%s\n", found->key, found->tag);

    pthread_t th[3];
    thread_arg_t args[3] = {
        {1, 10},
        {2, 10},
        {3, 10},
    };
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&th[i], NULL, worker_fn, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    uint32_t ret_sum = 0;
    for (int i = 0; i < 3; i++) {
        void *ret = NULL;
        if (pthread_join(th[i], &ret) != 0) {
            fprintf(stderr, "pthread_join failed\n");
            return 1;
        }
        ret_sum += (uint32_t)(uintptr_t)ret;
    }
    /* Expected: (1+2+3)*55 = 330; ret_sum = 330 + 3 = 333 */
    if (g_thread_sum != 330u || ret_sum != 333u) {
        fprintf(stderr, "pthread sum mismatch: %u / %u\n", g_thread_sum, ret_sum);
        return 1;
    }
    printf("PTHREAD_OK sum=%u ret=%u\n", g_thread_sum, ret_sum);
    fflush(stdout);

    if (atexit(atexit_handler_1) != 0 || atexit(atexit_handler_2) != 0) {
        fprintf(stderr, "atexit failed\n");
        return 1;
    }
    return 0;
}
