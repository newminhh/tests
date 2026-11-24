#!/usr/bin/env bash
set -euo pipefail

# Part 4 Verification Script (In-Process Runtime DBT: libnewminhh_dbt.so & newminhh-run)
# - If invoked on host: syncs src/common, src/dbt, tests/part4 to newminhh-x32 VM and runs --guest-run.
# - If invoked inside Debian x32 VM (--guest-run): builds, installs, and verifies all Part 4 criteria.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

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

run_guest_tests() {
    local arch
    arch="$(dpkg --print-architecture 2>/dev/null || uname -m)"
    if [[ "${arch}" != "x32" ]]; then
        echo "[ERROR] --guest-run must run inside Debian x32 VM (got: ${arch})" >&2
        exit 1
    fi

    local dbt_dir="${REPO_ROOT}/src/dbt"
    local part4_dir="${REPO_ROOT}/tests/part4"
    local i386_dir="${REPO_ROOT}/tests/corpus/build/i386"
    local x32_dir="${REPO_ROOT}/tests/corpus/build/x32"

    echo "=== [1/7] Building and installing libnewminhh_dbt.so & newminhh-run (0 warnings) ==="
    local build_log
    build_log="$(mktemp)"
    trap 'rm -f "${build_log:-}"' EXIT

    make -C "${dbt_dir}" clean all install 2>&1 | tee "${build_log}"
    if grep -E "warning:" "${build_log}"; then
        echo "[FAIL] Compiler warnings detected in src/dbt build!" >&2
        exit 1
    fi
    test -f /usr/local/lib/x86_64-linux-gnux32/libnewminhh_dbt.so
    test -x /usr/local/bin/newminhh-run
    readelf -l /usr/local/bin/newminhh-run | grep -q "0x70000000" || {
        echo "[FAIL] newminhh-run is not linked high at 0x70000000" >&2
        exit 1
    }
    if command -v newminhh-lookup-gen >/dev/null 2>&1; then
        newminhh-lookup-gen --system --quiet || true
    fi

    echo "=== [2/7] Running all 13 unpatched i386 CLI fixtures (C01-C11) under newminhh-run ==="
    pushd "${i386_dir}" >/dev/null
    for bin in "${CLI_LIST[@]}"; do
        local out_dbt out_ref
        out_dbt="$(timeout 10 newminhh-run "./${bin}")"
        out_ref="$(cd "${x32_dir}" && "./${bin}")"
        if [[ "${out_dbt}" != "${out_ref}" ]]; then
            echo "[FAIL] Output mismatch for ${bin}:" >&2
            echo "  DBT: ${out_dbt}" >&2
            echo "  X32: ${out_ref}" >&2
            popd >/dev/null
            exit 1
        fi
        echo "  [PASS] ${bin}: $(echo "${out_dbt}" | tr '\n' ' ')"
    done

    local env_out
    env_out="$(NEWMINHH_TEST_ENV=dbt_env_ok timeout 10 newminhh-run ./cli_libc_pie)"
    echo "${env_out}" | grep -q "env=dbt_env_ok" || {
        echo "[FAIL] NEWMINHH_TEST_ENV propagation failed: ${env_out}" >&2
        popd >/dev/null
        exit 1
    }
    echo "  [PASS] cli_libc_pie env propagation: ${env_out}"

    local bc_dbt bc_ref bl_dbt bl_ref
    bc_dbt="$(timeout 10 newminhh-run ./bench_workload --compute 100000)"
    bc_ref="$("${x32_dir}/bench_workload" --compute 100000)"
    [[ "${bc_dbt}" == "${bc_ref}" ]] || { echo "[FAIL] bench --compute mismatch" >&2; exit 1; }
    bl_dbt="$(timeout 10 newminhh-run ./bench_workload --libc-heavy 500)"
    bl_ref="$("${x32_dir}/bench_workload" --libc-heavy 500)"
    # ts.tv_nsec low bits vary; verify format and iters
    echo "${bl_dbt}" | grep -q "^BENCH_LIBC_OK csum=0x[0-9a-f]\{8\} iters=500$" || {
        echo "[FAIL] bench --libc-heavy unexpected output: ${bl_dbt}" >&2
        exit 1
    }
    echo "  [PASS] bench_workload (--compute & --libc-heavy): ${bc_dbt}"

    echo "=== [3/7] Testing newminhh-run --so direct invocation ==="
    set +e
    timeout 10 newminhh-run --so ./libplugin_stripped.so plugin_compute 7 5
    local so_rc=$?
    set -e
    if [[ "${so_rc}" -ne 47 ]]; then
        echo "[FAIL] newminhh-run --so plugin_compute(7, 5) returned ${so_rc}, expected 47" >&2
        popd >/dev/null
        exit 1
    fi
    echo "  [PASS] newminhh-run --so libplugin_stripped.so plugin_compute 7 5 -> 47"
    popd >/dev/null

    echo "=== [4/7] Running bidirectional .so host driver (S01-S03 + N09) via libnewminhh_dbt.so ==="
    local drv_out drv_ref
    drv_out="$(timeout 10 "${x32_dir}/x32_so_host_driver" --dbt "${i386_dir}")"
    drv_ref="$("${x32_dir}/x32_so_host_driver" "${x32_dir}")"
    if [[ "${drv_out}" != "${drv_ref}" ]]; then
        echo "[FAIL] x32_so_host_driver --dbt mismatch:" >&2
        echo "  DBT: ${drv_out}" >&2
        echo "  X32: ${drv_ref}" >&2
        exit 1
    fi
    echo "  [PASS] x32_so_host_driver --dbt: ${drv_out}"

    echo "=== [5/7] Verifying negative/malformed/unsupported inputs (N01-N09) fail loudly ==="
    for neg in "${NEG_LIST[@]}"; do
        local npath="${i386_dir}/negatives/${neg}"
        local nerr
        set +e
        nerr="$(timeout 10 newminhh-run "${npath}" 2>&1)"
        local nrc=$?
        set -e
        if [[ "${nrc}" -eq 0 ]]; then
            echo "[FAIL] Negative fixture ${neg} unexpectedly exited 0!" >&2
            exit 1
        fi
        if [[ -z "${nerr}" ]]; then
            echo "[FAIL] Negative fixture ${neg} produced no stderr diagnostic!" >&2
            exit 1
        fi
        echo "  [PASS] ${neg} (exit=${nrc}): $(echo "${nerr}" | head -n 1)"
    done

    set +e
    local n09_err
    n09_err="$(timeout 10 newminhh-run "${i386_dir}/neg_dlsym_missing" libc.so.6 newminhh_nonexistent_sym 2>&1)"
    local n09_rc=$?
    set -e
    if [[ "${n09_rc}" -eq 0 ]] || ! echo "${n09_err}" | grep -q "N09_EXPECTED_FAIL"; then
        echo "[FAIL] N09 neg_dlsym_missing failed to report expected missing-symbol error (rc=${n09_rc})" >&2
        exit 1
    fi
    echo "  [PASS] N09 neg_dlsym_missing (exit=${n09_rc})"

    echo "=== [6/7] Running Part 4 hybrid fallback API & MT stress tests ==="
    make -C "${part4_dir}" clean all 2>&1 | tee -a "${build_log}"
    if grep -E "warning:" "${build_log}"; then
        echo "[FAIL] Compiler warnings detected in tests/part4 build!" >&2
        exit 1
    fi
    timeout 10 "${part4_dir}/test_part4_api" "${i386_dir}/cli_switch_jumptable_pie"
    timeout 30 "${part4_dir}/test_part4_mt_stress" "${i386_dir}"

    echo "=== [7/7] Running Part 4 native libz round-trip test under newminhh-run ==="
    local z_bin="${part4_dir}/test_part4_native_libz"
    if [[ ! -x "${z_bin}" ]]; then
        z_bin="${part4_dir}/guest_native_libz"
    fi
    local z_out
    z_out="$(timeout 10 newminhh-run "${z_bin}")"
    echo "${z_out}" | grep -q "ZLIB_NATIVE_ROUNDTRIP_OK" || {
        echo "[FAIL] test_part4_native_libz unexpected output: ${z_out}" >&2
        exit 1
    }
    echo "  [PASS] test_part4_native_libz: ${z_out}"

    echo "=== ALL PART 4 VERIFICATION CHECKS PASSED ==="
}

if [[ "${1:-}" == "--guest-run" ]]; then
    run_guest_tests
    exit 0
fi

VM_EXEC="${REPO_ROOT}/vm/vm-exec.sh"
if [[ ! -x "${VM_EXEC}" ]]; then
    echo "[ERROR] ${VM_EXEC} not found or not executable" >&2
    exit 1
fi

echo "=== Building host i386 native-libz guest fixture ==="
make -C "${REPO_ROOT}/tests/part4" CC=gcc i386
trap 'rm -f "${REPO_ROOT}/tests/part4/test_part4_native_libz" "${REPO_ROOT}/tests/part4/guest_native_libz"' EXIT

echo "=== Syncing src/common, src/dbt, and tests/part4 to newminhh-x32 VM ==="
"${VM_EXEC}" --sync "${REPO_ROOT}/src/common" /root/newminhh/src/common
"${VM_EXEC}" --sync "${REPO_ROOT}/src/dbt" /root/newminhh/src/dbt
"${VM_EXEC}" --sync "${REPO_ROOT}/tests/part4" /root/newminhh/tests/part4

echo "=== Executing Part 4 verification inside newminhh-x32 VM ==="
"${VM_EXEC}" -- bash /root/newminhh/tests/part4/run_part4_tests.sh --guest-run
