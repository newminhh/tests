#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* i386 zlib function prototypes */
extern const char *zlibVersion(void);
extern unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len);
extern int compress2(unsigned char *dest, unsigned long *destLen,
                     const unsigned char *source, unsigned long sourceLen,
                     int level);
extern int uncompress(unsigned char *dest, unsigned long *destLen,
                      const unsigned char *source, unsigned long sourceLen);

#define TEST_DATA_LEN 1024u

int main(void) {
    /* 1. Verify zlibVersion() */
    const char *ver = zlibVersion();
    if (!ver || ver[0] == '\0') {
        fprintf(stderr, "FAIL: zlibVersion() returned NULL or empty\n");
        return 1;
    }
    printf("zlibVersion: %s\n", ver);

    /* 2. Prepare test payload */
    unsigned char orig[TEST_DATA_LEN];
    for (size_t i = 0; i < TEST_DATA_LEN; i++) {
        orig[i] = (unsigned char)((i * 37u + 13u) & 0xFFu);
    }

    /* 3. Compute initial CRC32 */
    unsigned long orig_crc = crc32(0UL, orig, (unsigned int)TEST_DATA_LEN);
    if (orig_crc == 0) {
        fprintf(stderr, "FAIL: initial crc32 returned 0\n");
        return 1;
    }

    /* 4. Compress buffer using compress2 */
    unsigned long comp_len = (unsigned long)(TEST_DATA_LEN + 128u);
    unsigned char *comp_buf = (unsigned char *)malloc((size_t)comp_len);
    if (!comp_buf) {
        fprintf(stderr, "FAIL: malloc comp_buf failed\n");
        return 1;
    }

    int rc = compress2(comp_buf, &comp_len, orig, (unsigned long)TEST_DATA_LEN, 6);
    if (rc != 0) {
        fprintf(stderr, "FAIL: compress2 failed with rc=%d\n", rc);
        free(comp_buf);
        return 1;
    }
    if (comp_len == 0 || comp_len >= (unsigned long)TEST_DATA_LEN) {
        fprintf(stderr, "FAIL: invalid compressed length %lu (orig=%u)\n",
                comp_len, TEST_DATA_LEN);
        free(comp_buf);
        return 1;
    }

    /* 5. Uncompress buffer using uncompress */
    unsigned long decomp_len = (unsigned long)TEST_DATA_LEN;
    unsigned char *decomp_buf = (unsigned char *)malloc((size_t)decomp_len);
    if (!decomp_buf) {
        fprintf(stderr, "FAIL: malloc decomp_buf failed\n");
        free(comp_buf);
        return 1;
    }
    memset(decomp_buf, 0, (size_t)decomp_len);

    rc = uncompress(decomp_buf, &decomp_len, comp_buf, comp_len);
    if (rc != 0) {
        fprintf(stderr, "FAIL: uncompress failed with rc=%d\n", rc);
        free(comp_buf);
        free(decomp_buf);
        return 1;
    }
    if (decomp_len != (unsigned long)TEST_DATA_LEN) {
        fprintf(stderr, "FAIL: uncompressed length mismatch (%lu != %u)\n",
                decomp_len, TEST_DATA_LEN);
        free(comp_buf);
        free(decomp_buf);
        return 1;
    }

    /* 6. Verify data integrity and CRC32 */
    if (memcmp(orig, decomp_buf, (size_t)TEST_DATA_LEN) != 0) {
        fprintf(stderr, "FAIL: decompressed data does not match original\n");
        free(comp_buf);
        free(decomp_buf);
        return 1;
    }

    unsigned long decomp_crc = crc32(0UL, decomp_buf, (unsigned int)decomp_len);
    if (decomp_crc != orig_crc) {
        fprintf(stderr, "FAIL: CRC mismatch: orig=0x%08lx decomp=0x%08lx\n",
                orig_crc, decomp_crc);
        free(comp_buf);
        free(decomp_buf);
        return 1;
    }

    free(comp_buf);
    free(decomp_buf);

    printf("ZLIB_NATIVE_ROUNDTRIP_OK: ver=%s crc=0x%08lx orig_sz=%u comp_sz=%lu\n",
           ver, orig_crc, TEST_DATA_LEN, comp_len);
    return 0;
}
