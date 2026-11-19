#!/usr/bin/env bash
set -euo pipefail

# Part 1 Corpus Verification Script
# Can be run:
#   1) On host: builds i386 corpus, syncs to Debian x32 VM via vm/vm-exec.sh, and invokes --guest-run inside VM
#   2) Inside Debian x32 guest with `--guest-run`: builds x32 baselines and runs the full Part 1 acceptance check.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CLI_LIST=(
    cli_libc_pie
    cli_libc_nopie
    cli_switch_jumptable_pie
    cli_switch_jumptable_nopie
    cli_pic_thunks
    cli_structs_abi
    cli_structs_abi64
    cli_callbacks
    cli_syscall_int80
    cli_syscall_sysenter
    cli_init_fini
    cli_dlopen_client
    cli_stripped
)

SO_LIST=(
    libbasic_pic.so
    liblifecycle_abi.so
    libplugin_stripped.so
)

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

run_guest_checks() {
    local arch
    arch="$(dpkg --print-architecture 2>/dev/null || uname -m)"
    if [[ "${arch}" != "x32" ]]; then
        echo "[ERROR] --guest-run must be executed inside the Debian x32 VM (got: ${arch})" >&2
        exit 1
    fi

    echo "=== [1/5] Building native x32 baselines in ${SCRIPT_DIR} ==="
    make -C "${SCRIPT_DIR}" x32

    local i386_dir="${SCRIPT_DIR}/build/i386"
    local x32_dir="${SCRIPT_DIR}/build/x32"

    echo "=== [2/5] Verifying ELF headers (i386 vs x32) ==="
    for bin in "${CLI_LIST[@]}" bench_workload neg_dlsym_missing; do
        local f_i386="${i386_dir}/${bin}"
        local f_x32="${x32_dir}/${bin}"
        test -x "${f_i386}" || { echo "Missing i386 binary: ${f_i386}" >&2; exit 1; }
        test -x "${f_x32}" || { echo "Missing x32 binary: ${f_x32}" >&2; exit 1; }
        readelf -h "${f_i386}" | grep -q "Class:.*ELF32"
        readelf -h "${f_i386}" | grep -q "Machine:.*Intel 80386"
        readelf -h "${f_x32}" | grep -q "Class:.*ELF32"
        readelf -h "${f_x32}" | grep -q "Machine:.*Advanced Micro Devices X86-64"
    done
    for so in "${SO_LIST[@]}"; do
        readelf -h "${i386_dir}/${so}" | grep -q "Machine:.*Intel 80386"
        readelf -h "${x32_dir}/${so}" | grep -q "Machine:.*Advanced Micro Devices X86-64"
    done
    for neg in "${NEG_LIST[@]}"; do
        test -s "${i386_dir}/negatives/${neg}" || { echo "Missing negative fixture: ${neg}" >&2; exit 1; }
    done
    echo "ELF headers verified."

    echo "=== [3/5] Running native x32 baselines (expect PASS) ==="
    pushd "${x32_dir}" >/dev/null
    for bin in "${CLI_LIST[@]}"; do
        echo "--- x32 baseline: ${bin} ---"
        "./${bin}"
    done
    echo "--- x32 baseline: x32_so_host_driver ---"
    ./x32_so_host_driver "${x32_dir}"
    echo "--- x32 baseline: bench_workload (--compute & --libc-heavy) ---"
    ./bench_workload --compute 100000
    ./bench_workload --libc-heavy 500
    echo "--- x32 negative N09: neg_dlsym_missing (expect non-zero exit) ---"
    if ./neg_dlsym_missing libc.so.6 newminhh_nonexistent_sym; then
        echo "[FAIL] N09 neg_dlsym_missing unexpectedly succeeded!" >&2
        popd >/dev/null
        exit 1
    fi
    popd >/dev/null

    echo "=== [4/5] Verifying un-translated i386 CLI fixtures FAIL on x32-only guest ==="
    for bin in "${CLI_LIST[@]}"; do
        local f_i386="${i386_dir}/${bin}"
        if "${f_i386}" >/dev/null 2>&1; then
            echo "[FAIL] Un-translated i386 binary ${bin} unexpectedly succeeded on x32-only guest!" >&2
            exit 1
        else
            echo "  [OK] ${bin} failed as expected (exit $?)"
        fi
    done

    echo "=== [5/5] Part 1 Corpus Acceptance Check PASSED ==="
}

if [[ "${1:-}" == "--guest-run" ]]; then
    run_guest_checks
    exit 0
fi

echo "=== Building i386 corpus on host ==="
make -C "${SCRIPT_DIR}" verify-i386

VM_EXEC="${REPO_ROOT}/vm/vm-exec.sh"
if [[ ! -x "${VM_EXEC}" ]]; then
    echo "[ERROR] ${VM_EXEC} not found or not executable" >&2
    exit 1
fi

echo "=== Syncing corpus to Debian x32 VM and running guest verification ==="
"${VM_EXEC}" --sync "${SCRIPT_DIR}" /root/newminhh/tests/corpus
"${VM_EXEC}" -- bash /root/newminhh/tests/corpus/run_part1_tests.sh --guest-run
