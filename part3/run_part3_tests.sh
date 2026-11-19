#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PATCHER="${REPO_ROOT}/src/patcher/newminhh-patch"
I386_DIR="${REPO_ROOT}/tests/corpus/build/i386"
NEG_DIR="${I386_DIR}/negatives"
VM_EXEC="${REPO_ROOT}/vm/vm-exec.sh"
REMOTE_X32_BASE="/root/newminhh/tests/corpus/build/x32"

[ -x "${PATCHER}" ] || { echo "ERROR: patcher missing/not executable: ${PATCHER}" >&2; exit 1; }
[ -d "${I386_DIR}" ] || { echo "ERROR: i386 corpus missing: ${I386_DIR}" >&2; exit 1; }
[ -x "${VM_EXEC}" ] || { echo "ERROR: vm-exec missing: ${VM_EXEC}" >&2; exit 1; }

WORK_DIR="$(mktemp -d -t newminhh_part3_XXXXXX)"
REMOTE_DIR="/tmp/newminhh_part3_vm_$$"
cleanup() {
    rm -rf "${WORK_DIR}"
    "${VM_EXEC}" -- rm -rf "${REMOTE_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "${WORK_DIR}/static" "${WORK_DIR}/hybrid" "${WORK_DIR}/re_tools" "${WORK_DIR}/neg"

PASS_COUNT=0
FAIL_COUNT=0

pass() {
    echo "[PASS] $*"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    echo "[FAIL] $*" >&2
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

verify_elf_x32() {
    local path="$1"
    local mode="$2"
    local is_exec="$3"
    local hdr dyn phdr
    hdr="$(readelf -h "${path}")"
    dyn="$(readelf -d "${path}")"

    if ! grep -qE 'Class:[[:space:]]+ELF32' <<<"${hdr}"; then
        fail "readelf -h (${mode}) $(basename "${path}"): expected Class ELF32"
        return 1
    fi
    if ! grep -qE 'Machine:[[:space:]]+Advanced Micro Devices X86-64' <<<"${hdr}"; then
        fail "readelf -h (${mode}) $(basename "${path}"): expected Machine Advanced Micro Devices X86-64"
        return 1
    fi
    if ! grep -qE '\(RELA\)' <<<"${dyn}"; then
        fail "readelf -d (${mode}) $(basename "${path}"): missing DT_RELA"
        return 1
    fi
    if ! grep -qE 'Shared library:[[:space:]]+\[libnewminhh_compat\.so\]' <<<"${dyn}"; then
        fail "readelf -d (${mode}) $(basename "${path}"): missing libnewminhh_compat.so in DT_NEEDED"
        return 1
    fi
    if [ "${mode}" = "static" ] && grep -qE 'Shared library:[[:space:]]+\[libnewminhh_dbt\.so\]' <<<"${dyn}"; then
        fail "readelf -d (${mode}) $(basename "${path}"): unexpected libnewminhh_dbt.so under --no-hybrid-fallback"
        return 1
    fi
    if [ "${mode}" = "hybrid" ] && ! grep -qE 'Shared library:[[:space:]]+\[libnewminhh_dbt\.so\]' <<<"${dyn}"; then
        fail "readelf -d (${mode}) $(basename "${path}"): missing libnewminhh_dbt.so under --hybrid-fallback / fallback"
        return 1
    fi
    if [ "${is_exec}" = "1" ]; then
        phdr="$(readelf -l "${path}")"
        if ! grep -q '/libx32/ld-linux-x32.so.2' <<<"${phdr}"; then
            fail "readelf -l (${mode}) $(basename "${path}"): missing PT_INTERP /libx32/ld-linux-x32.so.2"
            return 1
        fi
    fi
    pass "ELF32/x32 header+dynamic check (${mode}/$(basename "${path}"))"
}

echo "=== Stage 1: Patching all 18 i386 corpus ELFs (--no-hybrid-fallback + --hybrid-fallback) ==="
CORPUS_ELFS=()
for f in "${I386_DIR}"/*; do
    [ -f "${f}" ] || continue
    b="$(basename "${f}")"
    case "${b}" in
        *.o) continue ;;
    esac
    CORPUS_ELFS+=("${b}")

    is_exec=1
    case "${b}" in
        *.so) is_exec=0 ;;
    esac

    if "${PATCHER}" --no-hybrid-fallback "${f}" -o "${WORK_DIR}/static/${b}"; then
        verify_elf_x32 "${WORK_DIR}/static/${b}" "static" "${is_exec}"
    else
        fail "newminhh-patch --no-hybrid-fallback failed on ${b}"
    fi

    if "${PATCHER}" --hybrid-fallback "${f}" -o "${WORK_DIR}/hybrid/${b}"; then
        verify_elf_x32 "${WORK_DIR}/hybrid/${b}" "hybrid" "${is_exec}"
    else
        fail "newminhh-patch --hybrid-fallback failed on ${b}"
    fi
done

echo "=== Stage 1b: REQUIRES_DBT_FALLBACK fixture (PIE & non-PIE, auto + --hybrid-fallback + --no-hybrid-fallback) ==="
mkdir -p "${WORK_DIR}/fallback"
FB_SRC="${SCRIPT_DIR}/fb_fallback_test.c"
[ -f "${FB_SRC}" ] || { echo "ERROR: missing ${FB_SRC}" >&2; exit 1; }
gcc -m32 -O2 -fPIE -pie "${FB_SRC}" -o "${WORK_DIR}/fb_pie.i386"
gcc -m32 -O2 -fno-pie -no-pie "${FB_SRC}" -o "${WORK_DIR}/fb_nopie.i386"

for variant in fb_pie fb_nopie; do
    src_fb="${WORK_DIR}/${variant}.i386"
    # Must fail loudly under --no-hybrid-fallback
    rm -f "${WORK_DIR}/fallback/${variant}.static"
    set +e
    fb_diag="$("${PATCHER}" --no-hybrid-fallback "${src_fb}" -o "${WORK_DIR}/fallback/${variant}.static" 2>&1 >/dev/null)"
    fb_rc=$?
    set -e
    if [ "${fb_rc}" -ne 0 ] && [ ! -e "${WORK_DIR}/fallback/${variant}.static" ] && grep -q 'require DBT fallback' <<<"${fb_diag}"; then
        pass "${variant}: --no-hybrid-fallback rejected REQUIRES_DBT_FALLBACK cleanly"
    else
        fail "${variant}: --no-hybrid-fallback failed to reject REQUIRES_DBT_FALLBACK (rc=${fb_rc}, diag=${fb_diag})"
    fi

    # Patch in default auto mode (must auto-link libnewminhh_dbt.so)
    set +e
    auto_log="$("${PATCHER}" -v "${src_fb}" -o "${WORK_DIR}/fallback/${variant}.auto" 2>&1)"
    auto_rc=$?
    set -e
    if [ "${auto_rc}" -eq 0 ] && grep -qE 'fallback=[1-9]' <<<"${auto_log}"; then
        verify_elf_x32 "${WORK_DIR}/fallback/${variant}.auto" "hybrid" "1"
    else
        fail "${variant}: auto mode failed or missed REQUIRES_DBT_FALLBACK (rc=${auto_rc}, log=${auto_log})"
    fi

    # Patch with explicit --hybrid-fallback
    set +e
    hyb_log="$("${PATCHER}" -v --hybrid-fallback "${src_fb}" -o "${WORK_DIR}/fallback/${variant}.hyb" 2>&1)"
    hyb_rc=$?
    set -e
    if [ "${hyb_rc}" -eq 0 ] && grep -qE 'fallback=[1-9]' <<<"${hyb_log}"; then
        verify_elf_x32 "${WORK_DIR}/fallback/${variant}.hyb" "hybrid" "1"
    else
        fail "${variant}: --hybrid-fallback failed or missed REQUIRES_DBT_FALLBACK (rc=${hyb_rc}, log=${hyb_log})"
    fi
done

# Verify non-PLT indirect jmp *(%reg) in a GOT-equipped binary is flagged REQUIRES_DBT_FALLBACK (Finding 4)
cat >"${WORK_DIR}/mem_ijmp.c" <<'EOF_MEM_IJMP'
#include <stdio.h>
__attribute__((noinline)) static int target_fn(int x) { return x + 42; }
__attribute__((noinline)) static int via_mem_ijmp(int (**fp)(int), int x) {
    int r;
    __asm__ volatile("pushl %2; call 1f; 1: addl $(2f-1b), (%%esp); movl %1, %%eax; jmp *(%%eax); 2: addl $4, %%esp"
                     : "=a"(r) : "m"(fp), "r"(x) : "ecx", "edx", "cc", "memory");
    return r;
}
int main(void) {
    int (*fn)(int) = target_fn;
    printf("MEM_IJMP=%d\n", via_mem_ijmp(&fn, 8));
    return 0;
}
EOF_MEM_IJMP
gcc -m32 -O2 -fPIE -pie "${WORK_DIR}/mem_ijmp.c" -o "${WORK_DIR}/mem_ijmp.i386"
set +e
mem_diag="$("${PATCHER}" --no-hybrid-fallback "${WORK_DIR}/mem_ijmp.i386" -o "${WORK_DIR}/fallback/mem_ijmp.static" 2>&1 >/dev/null)"
mem_rc=$?
set -e
if [ "${mem_rc}" -ne 0 ] && [ ! -e "${WORK_DIR}/fallback/mem_ijmp.static" ] && grep -q 'require DBT fallback' <<<"${mem_diag}"; then
    pass "mem_ijmp: non-PLT jmp *(%eax) cleanly flagged as REQUIRES_DBT_FALLBACK"
else
    fail "mem_ijmp: non-PLT jmp *(%eax) was misclassified as PLT (rc=${mem_rc}, diag=${mem_diag})"
fi

echo "=== Stage 2: --use-r2 & --use-ghidra stripped binary RE verification ==="
if "${PATCHER}" --use-r2 --no-hybrid-fallback "${I386_DIR}/cli_stripped" -o "${WORK_DIR}/re_tools/cli_stripped_r2"; then
    verify_elf_x32 "${WORK_DIR}/re_tools/cli_stripped_r2" "static" "1"
else
    fail "newminhh-patch --use-r2 failed on cli_stripped"
fi

if "${PATCHER}" --use-ghidra --no-hybrid-fallback "${I386_DIR}/libplugin_stripped.so" -o "${WORK_DIR}/re_tools/libplugin_ghidra.so"; then
    verify_elf_x32 "${WORK_DIR}/re_tools/libplugin_ghidra.so" "static" "0"
else
    fail "newminhh-patch --use-ghidra failed on libplugin_stripped.so"
fi

echo "=== Stage 3: Syncing patched artifacts to newminhh-x32 VM ==="
"${VM_EXEC}" -- mkdir -p "${REMOTE_DIR}/static" "${REMOTE_DIR}/hybrid" "${REMOTE_DIR}/re_tools" "${REMOTE_DIR}/fallback"
tar -C "${WORK_DIR}" -cf - static hybrid re_tools fallback | "${VM_EXEC}" -- tar -C "${REMOTE_DIR}" -xf -

echo "=== Stage 4: Direct native x32 kernel + ld-linux-x32.so.2 execution inside newminhh-x32 ==="
set +e
VM_RESULTS="$(
"${VM_EXEC}" -- bash -s "${REMOTE_DIR}" "${REMOTE_X32_BASE}" <<'EOF_VM'
set -u
RDIR="$1"
X32DIR="$2"

CLI_LIST=(
    cli_libc_pie
    cli_libc_nopie
    cli_switch_jumptable_pie
    cli_switch_jumptable_nopie
    cli_pic_thunks
    cli_init_fini
    cli_structs_abi
    cli_structs_abi64
    cli_callbacks
    cli_syscall_int80
    cli_syscall_sysenter
    cli_stripped
)

for mode in static hybrid; do
    for c in "${CLI_LIST[@]}"; do
        out_p="$("${RDIR}/${mode}/${c}" 2>&1)"
        rc_p=$?
        out_n="$("${X32DIR}/${c}" 2>&1)"
        rc_n=$?
        if [ "${rc_p}" -eq "${rc_n}" ] && [ "${out_p}" = "${out_n}" ]; then
            echo "VM_PASS ${mode}/${c}"
        else
            echo "VM_FAIL ${mode}/${c} rc_p=${rc_p} rc_n=${rc_n}"
            echo "  GOT: ${out_p}"
            echo "  EXP: ${out_n}"
        fi
    done

    # Environment variable check
    out_env="$(NEWMINHH_TEST_ENV="part3_env_ok" "${RDIR}/${mode}/cli_libc_pie" 2>&1)"
    if grep -qF "env=part3_env_ok" <<<"${out_env}"; then
        echo "VM_PASS ${mode}/env_propagation"
    else
        echo "VM_FAIL ${mode}/env_propagation GOT=${out_env}"
    fi

    # cli_dlopen_client (loads ./libplugin_stripped.so + libm.so.6)
    out_p="$(cd "${RDIR}/${mode}" && ./cli_dlopen_client 2>&1)"
    rc_p=$?
    out_n="$(cd "${X32DIR}" && ./cli_dlopen_client 2>&1)"
    rc_n=$?
    if [ "${rc_p}" -eq "${rc_n}" ] && [ "${out_p}" = "${out_n}" ]; then
        echo "VM_PASS ${mode}/cli_dlopen_client"
    else
        echo "VM_FAIL ${mode}/cli_dlopen_client rc_p=${rc_p} rc_n=${rc_n}"
        echo "  GOT: ${out_p}"
        echo "  EXP: ${out_n}"
    fi

    # bench_workload
    out_p="$("${RDIR}/${mode}/bench_workload" --compute 100000 2>&1)"
    rc_p=$?
    out_n="$("${X32DIR}/bench_workload" --compute 100000 2>&1)"
    rc_n=$?
    if [ "${rc_p}" -eq "${rc_n}" ] && [ "${out_p}" = "${out_n}" ]; then
        echo "VM_PASS ${mode}/bench_workload"
    else
        echo "VM_FAIL ${mode}/bench_workload rc_p=${rc_p} rc_n=${rc_n}"
    fi

    # N09 neg_dlsym_missing
    set +e
    out_n09="$("${RDIR}/${mode}/neg_dlsym_missing" 2>&1)"
    rc_n09=$?
    set -e
    if [ "${rc_n09}" -ne 0 ] && grep -qF "N09_EXPECTED_FAIL" <<<"${out_n09}"; then
        echo "VM_PASS ${mode}/neg_dlsym_missing"
    else
        echo "VM_FAIL ${mode}/neg_dlsym_missing rc=${rc_n09} out=${out_n09}"
    fi

    # x32_so_host_driver (native x32 dlopen/dlsym/dlclose on patched S01-S03)
    out_so_p="$("${X32DIR}/x32_so_host_driver" "${RDIR}/${mode}" 2>&1)"
    rc_so_p=$?
    out_so_n="$("${X32DIR}/x32_so_host_driver" "${X32DIR}" 2>&1)"
    rc_so_n=$?
    if [ "${rc_so_p}" -eq "${rc_so_n}" ] && [ "${out_so_p}" = "${out_so_n}" ]; then
        echo "VM_PASS ${mode}/x32_so_host_driver (${out_so_p})"
    else
        echo "VM_FAIL ${mode}/x32_so_host_driver rc_p=${rc_so_p} rc_n=${rc_so_n} GOT=[${out_so_p}] EXP=[${out_so_n}]"
    fi
done

# REQUIRES_DBT_FALLBACK execution checks (auto + --hybrid-fallback, PIE + non-PIE)
FB_EXP="FB_OK indirect=76 direct=89 esi=0x11223344 edi=0x55667788"
for fb_bin in fb_pie.auto fb_pie.hyb fb_nopie.auto fb_nopie.hyb; do
    out_fb="$("${RDIR}/fallback/${fb_bin}" 2>&1)"
    rc_fb=$?
    if [ "${rc_fb}" -eq 0 ] && [ "${out_fb}" = "${FB_EXP}" ]; then
        echo "VM_PASS fallback/${fb_bin} (${out_fb})"
    else
        echo "VM_FAIL fallback/${fb_bin} rc=${rc_fb} GOT=[${out_fb}] EXP=[${FB_EXP}]"
    fi
done

# r2-assisted stripped CLI check
out_r2="$("${RDIR}/re_tools/cli_stripped_r2" 2>&1)"
out_r2_n="$("${X32DIR}/cli_stripped" 2>&1)"
if [ "${out_r2}" = "${out_r2_n}" ]; then
    echo "VM_PASS re_tools/cli_stripped_r2"
else
    echo "VM_FAIL re_tools/cli_stripped_r2 GOT=${out_r2} EXP=${out_r2_n}"
fi

# ghidra-assisted stripped .so execution check via x32_so_host_driver
mkdir -p "${RDIR}/re_tools/so_check"
cp "${RDIR}/static/libbasic_pic.so" "${RDIR}/re_tools/so_check/libbasic_pic.so"
cp "${RDIR}/static/liblifecycle_abi.so" "${RDIR}/re_tools/so_check/liblifecycle_abi.so"
cp "${RDIR}/re_tools/libplugin_ghidra.so" "${RDIR}/re_tools/so_check/libplugin_stripped.so"
out_gh="$("${X32DIR}/x32_so_host_driver" "${RDIR}/re_tools/so_check" 2>&1)"
rc_gh=$?
out_gh_n="$("${X32DIR}/x32_so_host_driver" "${X32DIR}" 2>&1)"
if [ "${rc_gh}" -eq 0 ] && [ "${out_gh}" = "${out_gh_n}" ]; then
    echo "VM_PASS re_tools/libplugin_ghidra.so (${out_gh})"
else
    echo "VM_FAIL re_tools/libplugin_ghidra.so rc=${rc_gh} GOT=[${out_gh}] EXP=[${out_gh_n}]"
fi
EOF_VM
)"
vm_rc=$?
set -e
if [ "${vm_rc}" -ne 0 ]; then
    fail "Stage 4 VM execution exited with rc=${vm_rc}"
fi

while IFS= read -r line; do
    case "${line}" in
        VM_PASS*)
            pass "${line#VM_PASS }"
            ;;
        VM_FAIL*)
            fail "${line#VM_FAIL }"
            ;;
        *)
            echo "  ${line}"
            ;;
    esac
done <<<"${VM_RESULTS}"

echo "=== Stage 5: Negative fixtures N01..N08 (must fail loudly, zero output file) ==="
NEG_LIST=(
    bad_trunc_ehdr.elf
    bad_magic.bin
    bad_arch_arm.elf
    bad_class64_i386.elf
    bad_phdr_oob.elf
    bad_corrupt_dyn.elf
    bad_priv_insn.elf
    bad_bcd_insn.elf
)

for n in "${NEG_LIST[@]}"; do
    src="${NEG_DIR}/${n}"
    out="${WORK_DIR}/neg/${n}.x32"
    [ -f "${src}" ] || { fail "missing negative fixture ${src}"; continue; }
    rm -f "${out}"
    set +e
    diag="$("${PATCHER}" "${src}" -o "${out}" 2>&1 >/dev/null)"
    rc=$?
    set -e
    if [ "${rc}" -eq 0 ]; then
        fail "negative ${n}: patcher unexpectedly exited 0"
        continue
    fi
    if [ -e "${out}" ]; then
        fail "negative ${n}: left corrupt output file ${out}"
        continue
    fi
    if ! grep -q '\[newminhh:patcher\] ERROR:' <<<"${diag}"; then
        fail "negative ${n}: missing '[newminhh:patcher] ERROR:' diagnostic in stderr: ${diag}"
        continue
    fi
    err_line="$(grep '\[newminhh:patcher\] ERROR:' <<<"${diag}" | head -n 1)"
    pass "negative ${n} (rc=${rc}): ${err_line}"
done

echo ""
echo "========================================"
echo "Part 3 Summary: PASS=${PASS_COUNT} FAIL=${FAIL_COUNT}"
echo "========================================"
[ "${FAIL_COUNT}" -eq 0 ]
