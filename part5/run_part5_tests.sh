#!/usr/bin/env bash
set -euo pipefail

# Part 5 End-to-End Verification Runner:
#   1. Reproducible clean build & install (0 compiler warnings)
#   2. bin/newminhh CLI subcommand & error-handling tests
#   3. Full Parts 1-4 regression verification
#   4. /proc/<pid>/smaps sharing, RSS/PSS & execution timing benchmarks in newminhh-x32

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
CLI="${REPO_ROOT}/bin/newminhh"
VM_EXEC="${REPO_ROOT}/vm/vm-exec.sh"
REMOTE_ROOT="/root/newminhh"

BENCH_ONLY=0
GUEST_RUN=0
PASS_THROUGH_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bench-only)
            BENCH_ONLY=1
            shift
            ;;
        --guest-run)
            GUEST_RUN=1
            shift
            ;;
        --)
            shift
            PASS_THROUGH_ARGS+=("$@")
            break
            ;;
        *)
            PASS_THROUGH_ARGS+=("$1")
            shift
            ;;
    esac
done

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

is_x32_env() {
    local darch=""
    darch="$(dpkg --print-architecture 2>/dev/null || true)"
    [[ "${darch}" == "x32" ]] && return 0
    if command -v gcc >/dev/null 2>&1 && gcc -dM -E - </dev/null 2>/dev/null | grep -q '__ILP32__'; then
        return 0
    fi
    return 1
}

if [[ "${GUEST_RUN}" -eq 1 ]]; then
    if ! is_x32_env; then
        echo "[newminhh:part5] ERROR: --guest-run must run inside Debian x32 environment" >&2
        exit 1
    fi
    if [[ "${BENCH_ONLY}" -eq 1 ]]; then
        if [[ ! -x /tmp/newminhh_bench_patched ]]; then
            echo "[newminhh:part5] ERROR: /tmp/newminhh_bench_patched not found; run 'make bench' on host to patch via re-env:latest" >&2
            exit 1
        fi
        HYBRID_ARGS=()
        if [[ -x /tmp/newminhh_bench_hybrid ]]; then
            HYBRID_ARGS=(--hybrid /tmp/newminhh_bench_hybrid)
        fi
        exec python3 "${SCRIPT_DIR}/bench_smaps.py" --patched /tmp/newminhh_bench_patched "${HYBRID_ARGS[@]}" "${PASS_THROUGH_ARGS[@]}"
    fi
    make -C "${REPO_ROOT}" clean build install
    make -C "${REPO_ROOT}" test-part1 test-part2 test-part4
    if [[ ! -x /tmp/newminhh_bench_patched ]]; then
        echo "[newminhh:part5] ERROR: /tmp/newminhh_bench_patched not found; run 'make bench' on host first to stage patched benchmark artifacts" >&2
        exit 1
    fi
    HYBRID_ARGS=()
    if [[ -x /tmp/newminhh_bench_hybrid ]]; then
        HYBRID_ARGS=(--hybrid /tmp/newminhh_bench_hybrid)
    fi
    python3 "${SCRIPT_DIR}/bench_smaps.py" --patched /tmp/newminhh_bench_patched "${HYBRID_ARGS[@]}" "${PASS_THROUGH_ARGS[@]}"
    exit 0
fi

WORK_DIR="$(mktemp -d -t newminhh_part5_XXXXXX)"
REMOTE_TMP="/tmp/newminhh_part5_vm_$$"
cleanup() {
    rm -rf "${WORK_DIR}"
    if [[ -x "${VM_EXEC}" ]]; then
        "${VM_EXEC}" -- rm -rf "${REMOTE_TMP}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

run_bench_stage() {
    local i386_bench="${REPO_ROOT}/tests/corpus/build/i386/bench_workload"
    if [[ ! -x "${i386_bench}" ]]; then
        if [[ "${BENCH_ONLY}" -eq 1 ]]; then
            make -C "${REPO_ROOT}/tests/corpus" i386 >&2
        else
            make -C "${REPO_ROOT}/tests/corpus" i386
        fi
    fi

    if [[ "${BENCH_ONLY}" -eq 1 ]]; then
        echo "=== [Part 5] Patching bench_workload via newminhh patch (auto + --hybrid-fallback) ===" >&2
    else
        echo "=== [Part 5] Patching bench_workload via newminhh patch (auto + --hybrid-fallback) ==="
    fi
    "${CLI}" patch "${i386_bench}" -o "${WORK_DIR}/bench_workload.patched"
    "${CLI}" patch --hybrid-fallback "${i386_bench}" -o "${WORK_DIR}/bench_workload.hybrid"
    if [[ "${BENCH_ONLY}" -eq 1 ]]; then
        echo "[PASS] patched bench_workload (standard & --hybrid-fallback)" >&2
    else
        pass "patched bench_workload (standard & --hybrid-fallback)"
    fi

    "${VM_EXEC}" --sync "${SCRIPT_DIR}" "${REMOTE_ROOT}/tests/part5" >/dev/null
    "${VM_EXEC}" -- mkdir -p "${REMOTE_TMP}"
    tar -C "${WORK_DIR}" -cf - bench_workload.patched bench_workload.hybrid | \
        "${VM_EXEC}" -- tar -C "${REMOTE_TMP}" -xf -
    "${VM_EXEC}" -- bash -c "cp -f $(printf '%q' "${REMOTE_TMP}/bench_workload.patched") /tmp/newminhh_bench_patched && cp -f $(printf '%q' "${REMOTE_TMP}/bench_workload.hybrid") /tmp/newminhh_bench_hybrid"

    if [[ "${BENCH_ONLY}" -eq 1 ]]; then
        echo "=== [Part 5] Running /proc/<pid>/smaps, RSS & execution timing benchmarks inside newminhh-x32 ===" >&2
    else
        echo "=== [Part 5] Running /proc/<pid>/smaps, RSS & execution timing benchmarks inside newminhh-x32 ==="
    fi
    "${VM_EXEC}" -- python3 "${REMOTE_ROOT}/tests/part5/bench_smaps.py" \
        --native "${REMOTE_ROOT}/tests/corpus/build/x32/bench_workload" \
        --patched "${REMOTE_TMP}/bench_workload.patched" \
        --hybrid "${REMOTE_TMP}/bench_workload.hybrid" \
        --i386 "${REMOTE_ROOT}/tests/corpus/build/i386/bench_workload" \
        --runner /usr/local/bin/newminhh-run \
        "${PASS_THROUGH_ARGS[@]}"
    if [[ "${BENCH_ONLY}" -eq 1 ]]; then
        echo "[PASS] bench_smaps.py table & smaps invariants" >&2
    else
        pass "bench_smaps.py table & smaps invariants"
    fi

    if [[ "${BENCH_ONLY}" -eq 0 ]]; then
        echo "=== [Part 5] Verifying '${CLI} bench --json' end-to-end output ==="
        "${CLI}" bench --samples 1 --daemon-sec 2 --json | tee "${WORK_DIR}/bench.json" | python3 -m json.tool >/dev/null
        python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
assert d['sharing_summary']['libc_inode'] > 0
assert d['sharing_summary']['compat_inode'] > 0
assert len(d['memory']) == 4
assert len(d['timings']) == 3
print('JSON_OK libc_inode=%d compat_inode=%d' % (d['sharing_summary']['libc_inode'], d['sharing_summary']['compat_inode']))
" "${WORK_DIR}/bench.json"
        pass "cli bench --samples 1 --daemon-sec 2 --json valid JSON & invariants"
    fi
}

if [[ "${BENCH_ONLY}" -eq 1 ]]; then
    run_bench_stage
    exit 0
fi

echo "=== Stage 1: Clean Build & Install Reproducibility (0 compiler warnings) ==="
BUILD_LOG="${WORK_DIR}/build.log"
make -C "${REPO_ROOT}" clean
make -C "${REPO_ROOT}" build 2>&1 | tee "${BUILD_LOG}"
if grep -E "(^| )warning:" "${BUILD_LOG}"; then
    fail "compiler warnings detected during 'make build'"
else
    pass "clean 'make build' produced 0 compiler warnings"
fi
make -C "${REPO_ROOT}" install
pass "make install succeeded"

"${VM_EXEC}" -- bash -euo pipefail -c '
test -f /usr/local/lib/x86_64-linux-gnux32/libnewminhh_compat.so
test -f /usr/local/lib/x86_64-linux-gnux32/libnewminhh_dbt.so
test -x /usr/local/bin/newminhh-run
test -x /usr/local/bin/newminhh
/usr/local/bin/newminhh --help >/dev/null
'
pass "installed x32 libraries, newminhh-run, and /usr/local/bin/newminhh verified inside VM"

echo "=== Stage 2: bin/newminhh CLI Verification ==="
chmod +x "${CLI}"
"${CLI}" --help >/dev/null && pass "cli --help"
"${CLI}" -h >/dev/null && pass "cli -h"

set +e
bad_cmd_err="$("${CLI}" nonexistent_subcommand 2>&1 >/dev/null)"
bad_cmd_rc=$?
set -e
if [[ "${bad_cmd_rc}" -ne 0 ]] && grep -q '\[newminhh\] ERROR:' <<<"${bad_cmd_err}"; then
    pass "cli invalid subcommand rejected (rc=${bad_cmd_rc})"
else
    fail "cli invalid subcommand did not fail properly (rc=${bad_cmd_rc}, err=${bad_cmd_err})"
fi

set +e
miss_run_err="$("${CLI}" run /nonexistent/elf_file 2>&1 >/dev/null)"
miss_run_rc=$?
set -e
if [[ "${miss_run_rc}" -ne 0 ]] && grep -q '\[newminhh\] ERROR:' <<<"${miss_run_err}"; then
    pass "cli run missing file rejected (rc=${miss_run_rc})"
else
    fail "cli run missing file did not fail properly (rc=${miss_run_rc}, err=${miss_run_err})"
fi

"${CLI}" vm verify >/dev/null && pass "cli vm verify"

"${CLI}" patch "${REPO_ROOT}/tests/corpus/build/i386/cli_libc_pie" -o "${WORK_DIR}/cli_libc_pie.x32"
test -x "${WORK_DIR}/cli_libc_pie.x32" && pass "cli patch cli_libc_pie"

set +e
neg_patch_err="$("${CLI}" patch "${REPO_ROOT}/tests/corpus/build/i386/negatives/bad_magic.bin" -o "${WORK_DIR}/bad_magic.out" 2>&1 >/dev/null)"
neg_patch_rc=$?
set -e
if [[ "${neg_patch_rc}" -ne 0 ]] && [[ ! -e "${WORK_DIR}/bad_magic.out" ]] && grep -q '\[newminhh:patcher\] ERROR:' <<<"${neg_patch_err}"; then
    pass "cli patch negative bad_magic.bin rejected loudly (rc=${neg_patch_rc})"
else
    fail "cli patch negative bad_magic.bin check failed (rc=${neg_patch_rc})"
fi

run_out="$("${CLI}" run "${REPO_ROOT}/tests/corpus/build/i386/cli_libc_pie")"
ref_out="$("${VM_EXEC}" -- /root/newminhh/tests/corpus/build/x32/cli_libc_pie)"
if [[ "${run_out}" == "${ref_out}" ]]; then
    pass "cli run cli_libc_pie matches x32 baseline (${run_out})"
else
    fail "cli run cli_libc_pie mismatch: got=${run_out} ref=${ref_out}"
fi

set +e
"${CLI}" run --so "${REPO_ROOT}/tests/corpus/build/i386/libplugin_stripped.so" plugin_compute 7 5 >/dev/null
so_rc=$?
set -e
if [[ "${so_rc}" -eq 47 ]]; then
    pass "cli run --so libplugin_stripped.so plugin_compute 7 5 preserved exit code 47"
else
    fail "cli run --so expected exit code 47, got ${so_rc}"
fi

set +e
neg_run_err="$("${CLI}" run "${REPO_ROOT}/tests/corpus/build/i386/negatives/bad_trunc_ehdr.elf" 2>&1)"
neg_run_rc=$?
set -e
if [[ "${neg_run_rc}" -ne 0 ]] && [[ -n "${neg_run_err}" ]]; then
    pass "cli run negative bad_trunc_ehdr.elf failed loudly (rc=${neg_run_rc})"
else
    fail "cli run negative bad_trunc_ehdr.elf did not fail properly (rc=${neg_run_rc})"
fi

if [[ "${NEWMINHH_SKIP_PARTS_1_TO_4:-0}" -ne 1 ]]; then
    echo "=== Stage 3: Full Parts 1-4 Regression Verification ==="
    "${REPO_ROOT}/vm/vm-verify-x32.sh" && pass "vm-verify-x32.sh"
    bash "${REPO_ROOT}/tests/corpus/run_part1_tests.sh" && pass "Part 1 corpus tests"
    bash "${REPO_ROOT}/tests/part2/run_part2_tests.sh" && pass "Part 2 compat tests"
    bash "${REPO_ROOT}/tests/part3/run_part3_tests.sh" && pass "Part 3 static/hybrid patcher tests"
    bash "${REPO_ROOT}/tests/part4/run_part4_tests.sh" && pass "Part 4 runtime DBT tests"
else
    echo "=== Stage 3: Skipping Parts 1-4 (NEWMINHH_SKIP_PARTS_1_TO_4=1) ==="
fi

echo "=== Stage 4: Part 5 /proc/<pid>/smaps, RSS & Execution Timing Benchmarks ==="
run_bench_stage

echo ""
echo "========================================"
echo "Part 5 Summary: PASS=${PASS_COUNT} FAIL=${FAIL_COUNT}"
echo "========================================"
[[ "${FAIL_COUNT}" -eq 0 ]]
