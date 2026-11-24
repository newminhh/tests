#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
LOOKUP_DIR="${REPO_ROOT}/src/lookup"
CC="${CC:-clang}"
SANITIZE="${SANITIZE:-}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "=== [1/6] Compiling and running test_lookup unit tests ==="
CFLAGS="-O2 -std=gnu23 -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wold-style-definition -Wshadow -Wundef -Wformat=2 -fstrict-flex-arrays=3 -Werror -I${REPO_ROOT}/src/common"
if [[ -n "${SANITIZE}" ]]; then
    CFLAGS="${CFLAGS} -fsanitize=${SANITIZE} -g"
fi
if "${CC}" --version 2>/dev/null | grep -qi clang; then
    CFLAGS="${CFLAGS} -Wnullability-completeness -Wno-nullability-extension -Wno-gcc-compat"
fi

${CC} ${CFLAGS} "${SCRIPT_DIR}/test_lookup.c" "${LOOKUP_DIR}/libnmlookup.a" -o "${TMP_DIR}/test_lookup"
"${TMP_DIR}/test_lookup"

echo "=== [2/6] Building test ELF DSO fixtures (x32, i386 decoy, LP64 decoy, patched DSO, fuzz inputs) ==="
SYSROOT="${TMP_DIR}/sysroot"
mkdir -p "${SYSROOT}/usr/lib" "${SYSROOT}/bin" "${SYSROOT}/etc"

# Create valid x32 shared library with unversioned and versioned symbols
cat << 'EOF' > "${TMP_DIR}/test_x32.c"
void good_x32_func(void) {}
void versioned_func(void) {}
int data_sym_x32 = 42;
EOF

cat << 'EOF' > "${TMP_DIR}/test_ver.map"
VER_1.0 {
    global:
        versioned_func;
};
EOF

gcc -mx32 -shared -nostdlib -fPIC -o "${SYSROOT}/usr/lib/libtest_x32.so" "${TMP_DIR}/test_x32.c" -Wl,--version-script="${TMP_DIR}/test_ver.map" -Wl,-soname,libtest_x32.so

# Create i386 decoy DSO (must be silently skipped!)
gcc -m32 -shared -nostdlib -fPIC -o "${SYSROOT}/usr/lib/libdecoy_i386.so" -x c - <<< "void decoy_i386_func(void) {}" -Wl,-soname,libdecoy_i386.so

# Create LP64 decoy DSO (must be silently skipped!)
gcc -m64 -shared -nostdlib -fPIC -o "${SYSROOT}/usr/lib/libdecoy_lp64.so" -x c - <<< "void decoy_lp64_func(void) {}" -Wl,-soname,libdecoy_lp64.so

# Create already-patched i386->x32 DSO containing .note.newminhh
cat << 'EOF' > "${TMP_DIR}/patched.c"
void my_patched_func(void) {}
__attribute__((section(".note.newminhh"), aligned(4), used))
static const struct {
    unsigned int namesz;
    unsigned int descsz;
    unsigned int type;
    char name[9];
    char pad[3];
    unsigned int desc[2];
} note = {
    .namesz = 9,
    .descsz = 8,
    .type = 0x4E4D4848,
    .name = "newminhh",
    .pad = {0},
    .desc = {2, 0}
};
EOF

gcc -mx32 -shared -nostdlib -fPIC -o "${SYSROOT}/usr/lib/libpatched.so" "${TMP_DIR}/patched.c" -Wl,-soname,libpatched.so

# Create fuzz / corrupted ELF files (must never crash!)
touch "${SYSROOT}/usr/lib/empty.so"
printf "\x7fELF\x01\x01\x01\x00" > "${SYSROOT}/usr/lib/trunc_hdr.so"
echo "GROUP ( /lib/libc.so )" > "${SYSROOT}/usr/lib/text_file.so"

cp "${SYSROOT}/usr/lib/libtest_x32.so" "${SYSROOT}/usr/lib/corrupt_phoff.so"
printf "\xff\xff\xff\xff" | dd of="${SYSROOT}/usr/lib/corrupt_phoff.so" bs=1 seek=28 conv=notrunc 2>/dev/null

cp "${SYSROOT}/usr/lib/libtest_x32.so" "${SYSROOT}/usr/lib/corrupt_shnum.so"
printf "\xff\xff" | dd of="${SYSROOT}/usr/lib/corrupt_shnum.so" bs=1 seek=48 conv=notrunc 2>/dev/null

echo "=== [3/6] Running newminhh-lookup-gen on sysroot ==="
OUT_TABLE="${TMP_DIR}/lookup_table.txt"
"${LOOKUP_DIR}/newminhh-lookup-gen" --sysroot "${SYSROOT}" --rules-dir "${LOOKUP_DIR}/rules.d" -o "${OUT_TABLE}" --quiet

# Verify table contents
grep -q "^#!newminhh-lookup v1$" "${OUT_TABLE}"
grep -q "^lib	libtest_x32.so	" "${OUT_TABLE}"
grep -q "^sym	libtest_x32.so	good_x32_func	libtest_x32.so	good_x32_func	UNTYPED	" "${OUT_TABLE}"
grep -q "^sym	libtest_x32.so	versioned_func	libtest_x32.so	versioned_func@VER_1.0	UNTYPED	" "${OUT_TABLE}"

# Verify decoys were skipped
if grep -q "libdecoy_i386.so" "${OUT_TABLE}" || grep -q "decoy_i386_func" "${OUT_TABLE}"; then
    echo "[FAIL] i386 decoy DSO was not skipped!" >&2
    exit 1
fi
if grep -q "libdecoy_lp64.so" "${OUT_TABLE}" || grep -q "decoy_lp64_func" "${OUT_TABLE}"; then
    echo "[FAIL] LP64 decoy DSO was not skipped!" >&2
    exit 1
fi

# Verify patched DSO produced PATCHED_CDECL with __nm_cdecl prefix
grep -q "^sym	libpatched.so	my_patched_func	libpatched.so	__nm_cdecl.my_patched_func	PATCHED_CDECL	" "${OUT_TABLE}"

echo "=== [4/6] Verifying --check command and staleness detection ==="
# Initial check: must succeed (exit 0)
"${LOOKUP_DIR}/newminhh-lookup-gen" --check "${OUT_TABLE}" --quiet

# Stale detection: touch sysroot directory
touch "${SYSROOT}/usr/lib"
if "${LOOKUP_DIR}/newminhh-lookup-gen" --check "${OUT_TABLE}" --quiet; then
    echo "[FAIL] --check should have failed on stale directory mtime!" >&2
    exit 1
fi

# Nonexistent file check: must exit 1
if "${LOOKUP_DIR}/newminhh-lookup-gen" --check "${TMP_DIR}/nonexistent.txt" --quiet; then
    echo "[FAIL] --check should have failed on nonexistent file!" >&2
    exit 1
fi

echo "=== [5/6] Verifying --emit-base, --emit-c-registry, and --emit-py-registry ==="
# Emit base table
BASE_TXT="${TMP_DIR}/lookup_base.txt"
"${LOOKUP_DIR}/newminhh-lookup-gen" --emit-base "${LOOKUP_DIR}/rules.d" -o "${BASE_TXT}" --quiet
grep -q "^#!newminhh-lookup v1$" "${BASE_TXT}"
grep -q "^lib	libz.so.1	" "${BASE_TXT}"
test "$(wc -l < "${BASE_TXT}")" -gt 5000

# Emit C registry
C_REG="${TMP_DIR}/compat_registry.gen.h"
# Create a sample 00-libc.txt in a test rules dir
RULES_SAMPLE="${TMP_DIR}/rules_sample"
mkdir -p "${RULES_SAMPLE}"
cat << 'EOF' > "${RULES_SAMPLE}/00-libc.txt"
#!newminhh-lookup v1
lib	libc.so.6	-	libc.so.6	-	NATIVE	-	-	-
sym	libc.so.6	malloc	libc.so.6	malloc	COMPAT	p:u	-	0
sym	libc.so.6	free	libc.so.6	free	COMPAT	v:p	-	0
sym	libc.so.6	printf	libc.so.6	printf	COMPAT	i:p...	VARIADIC,CDECL_DIRECT	0
sym	libc.so.6	stdin	-	-	COMPAT	D4	DATA	4
sym	libc.so.6	stat_only	-	-	COMPAT	i:p	STATIC_ONLY	0
EOF

"${LOOKUP_DIR}/newminhh-lookup-gen" --emit-c-registry "${RULES_SAMPLE}" -o "${C_REG}" --quiet
grep -q "g_sym_table" "${C_REG}"
grep -q "&compat_stdin" "${C_REG}"
grep -q "compat_printf_cdecl" "${C_REG}"
# Verify C registry compiles cleanly when included in a translation unit
cat << 'EOF' > "${TMP_DIR}/test_c_reg.c"
#include <stdio.h>
#include <stdint.h>
#include "newminhh_abi.h"

extern void compat_free(void *ptr);
extern void *compat_malloc(size_t sz);
extern void *compat_stdin;

#include "compat_registry.gen.h"
EOF

${CC} -std=gnu23 -fsyntax-only -I"${REPO_ROOT}/src/common" -I"${TMP_DIR}" "${TMP_DIR}/test_c_reg.c"

# Emit Python registry
PY_REG="${TMP_DIR}/_lookup_registry_gen.py"
"${LOOKUP_DIR}/newminhh-lookup-gen" --emit-py-registry "${RULES_SAMPLE}" -o "${PY_REG}" --quiet
python3 -m py_compile "${PY_REG}"

echo "=== [6/6] Verifying atomic file permissions (0644) ==="
PERMS="$(stat -c "%a" "${OUT_TABLE}")"
if [[ "${PERMS}" != "644" ]]; then
    echo "[FAIL] Expected file permissions 644, got: ${PERMS}" >&2
    exit 1
fi

echo "ALL_LOOKUP_TESTS_PASSED"
