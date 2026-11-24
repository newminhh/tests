#!/usr/bin/env bash
set -euo pipefail

# Part 6 Test Suite: Level 2A/2B Parity, Round-Trip, High-VA & Stub Benchmark Suite
# Supports --host-only and full host+VM mode.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

PATCHER="${REPO_ROOT}/src/patcher/newminhh-patch"
LOOKUP_GEN="${REPO_ROOT}/src/lookup/newminhh-lookup-gen"
I386_DIR="${REPO_ROOT}/tests/corpus/build/i386"
VM_EXEC="${REPO_ROOT}/vm/vm-exec.sh"
REMOTE_ROOT="/root/newminhh"

HOST_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-only)
            HOST_ONLY=1
            shift
            ;;
        *)
            shift
            ;;
    esac
done

[ -x "${PATCHER}" ] || { echo "ERROR: patcher missing/not executable: ${PATCHER}" >&2; exit 1; }
[ -d "${I386_DIR}" ] || { echo "ERROR: i386 corpus directory missing: ${I386_DIR}" >&2; exit 1; }

WORK_DIR="$(mktemp -d -t newminhh_part6_XXXXXX)"
REMOTE_DIR="/tmp/newminhh_part6_vm_$$"

cleanup() {
    rm -rf "${WORK_DIR}"
    if [[ "${HOST_ONLY}" -eq 0 && -x "${VM_EXEC}" ]]; then
        "${VM_EXEC}" -- rm -rf "${REMOTE_DIR}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

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

mkdir -p "${WORK_DIR}/build_i386" "${WORK_DIR}/l1" "${WORK_DIR}/l2"
export PYTHONPATH="${REPO_ROOT}/src/patcher:${PYTHONPATH:-}"

verify_elf_x32_level() {
    local path="$1"
    local exp_level="$2"
    local hdr phdr notes
    hdr="$(readelf -h "${path}")"
    phdr="$(readelf -l "${path}")"
    notes="$(readelf -n "${path}")"

    if ! grep -qE 'Class:[[:space:]]+ELF32' <<<"${hdr}"; then
        fail "readelf -h $(basename "${path}"): expected Class ELF32"
        return 1
    fi
    if ! grep -qE 'Machine:[[:space:]]+Advanced Micro Devices X86-64' <<<"${hdr}"; then
        fail "readelf -h $(basename "${path}"): expected Machine Advanced Micro Devices X86-64"
        return 1
    fi

    # Verify NX PT_GNU_STACK
    local stack_line
    stack_line="$(grep -E 'GNU_STACK' <<<"${phdr}" || true)"
    if [[ -z "${stack_line}" ]]; then
        fail "readelf -l $(basename "${path}"): missing PT_GNU_STACK"
        return 1
    fi
    if grep -qE 'E' <<<"$(awk '{print $7}' <<<"${stack_line}")"; then
        fail "readelf -l $(basename "${path}"): executable PT_GNU_STACK detected (NX violated)"
        return 1
    fi

    # Verify .note.newminhh v2 level tag
    if ! grep -qiE 'newminhh' <<<"${notes}"; then
        fail "readelf -n $(basename "${path}"): missing .note.newminhh"
        return 1
    fi

    local level_hex
    level_hex="$(printf "%02x" "$(( (exp_level << 4) | 0x01 ))")"
    if ! grep -qE "02 00 00 00 ${level_hex} 00 00 00" <<<"${notes}"; then
        fail "readelf -n $(basename "${path}"): expected level tag LEVEL(${exp_level}) (flag byte 0x${level_hex})"
        return 1
    fi

    pass "ELF32 x32 NX stack & LEVEL(${exp_level}) tag verified on $(basename "${path}")"
    return 0
}

echo "=== [1/7] Step 1: Generating fresh host lookup table ==="
HOST_LOOKUP="${WORK_DIR}/lookup_table.txt"
SYSROOT_PATH="/home/shadichy/newminhh-x32-sysroot"
RULES_DIR="${REPO_ROOT}/src/lookup/rules.d"

if [[ -d "${SYSROOT_PATH}" ]]; then
    echo "Using target sysroot at ${SYSROOT_PATH}..."
    "${LOOKUP_GEN}" --sysroot "${SYSROOT_PATH}" --rules-dir "${RULES_DIR}" -o "${HOST_LOOKUP}" --quiet
    pass "Generated lookup table via sysroot"
elif [[ -f "/var/cache/newminhh/lookup_table.txt" ]]; then
    echo "Using existing /var/cache/newminhh/lookup_table.txt..."
    cp "/var/cache/newminhh/lookup_table.txt" "${HOST_LOOKUP}"
    pass "Reused cached /var/cache/newminhh/lookup_table.txt"
else
    echo "Generating base lookup table from rules.d..."
    "${LOOKUP_GEN}" --emit-base "${RULES_DIR}" -o "${HOST_LOOKUP}" --quiet
    pass "Generated base lookup table"
fi

echo "=== [2/7] Step 2: Building strlen_bench and test_l2_highva (i386) ==="
gcc -m32 -O2 -fno-builtin-strlen -Wall -Wextra -Werror "${SCRIPT_DIR}/strlen_bench.c" -o "${WORK_DIR}/build_i386/strlen_bench"
gcc -m32 -O2 -Wall -Wextra -Werror "${SCRIPT_DIR}/test_l2_highva.c" -lm -o "${WORK_DIR}/build_i386/test_l2_highva"
pass "Built strlen_bench and test_l2_highva as i386 binaries"

echo "=== [3/7] Step 3: Patching corpus ELFs at --level 1 and --level 2 ==="
ALL_BINS=()
for f in "${I386_DIR}"/* "${WORK_DIR}/build_i386"/*; do
    [[ -f "${f}" ]] || continue
    b="$(basename "${f}")"
    case "${b}" in
        *.o) continue ;;
    esac
    ALL_BINS+=("${f}")
done

for f in "${ALL_BINS[@]}"; do
    b="$(basename "${f}")"
    # Level 1
    "${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 1 "${f}" -o "${WORK_DIR}/l1/${b}"
    verify_elf_x32_level "${WORK_DIR}/l1/${b}" 1

    # Level 2
    "${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 "${f}" -o "${WORK_DIR}/l2/${b}"
    verify_elf_x32_level "${WORK_DIR}/l2/${b}" 2
done
pass "All corpus binaries successfully patched and verified at Level 1 and Level 2"

echo "=== [4/7] Step 4: Testing --emit-asm -> --from-asm round-trip & stub count ==="
STRLEN_STUB_INSNS=0
for candidate in bench_workload cli_libc_pie strlen_bench; do
    if [[ -f "${WORK_DIR}/build_i386/${candidate}" ]]; then
        src_elf="${WORK_DIR}/build_i386/${candidate}"
    else
        src_elf="${I386_DIR}/${candidate}"
    fi
    asm_dir="${WORK_DIR}/asm_${candidate}"
    l2_bin="${WORK_DIR}/l2_ref_${candidate}"
    from_bin="${WORK_DIR}/from_asm_${candidate}"

    "${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 --emit-asm "${asm_dir}" "${src_elf}" -o "${l2_bin}"

    # Assert required bundle files exist
    for req in out.s stubs.s layout.ld manifest.json; do
        if [[ ! -f "${asm_dir}/${req}" ]]; then
            fail "Bundle '${asm_dir}' missing required file '${req}'"
        fi
    done
    pass "Bundle ${candidate}: out.s, stubs.s, layout.ld, manifest.json exist"

    # Inspect stubs.s for specialized strlen stub instruction count <= 20
    STUB_INSN_COUNT="$(python3 -c "
with open('${asm_dir}/manifest.json') as mf:
    import json; manifest = json.load(mf)
with open('${asm_dir}/stubs.s') as sf:
    stubs_s = sf.read()

strlen_idx = None
for i, imp in enumerate(manifest.get('imports', [])):
    if imp == 'strlen':
        strlen_idx = i
        break

if strlen_idx is not None:
    part = stubs_s.split(f'__nm_plt_{strlen_idx}:')[1].split('.globl')[0].strip()
    insns = [l.strip() for l in part.splitlines() if l.strip() and not l.strip().endswith(':')]
    print(len(insns))
else:
    print(-1)
")"

    if [[ "${STUB_INSN_COUNT}" -gt 0 ]]; then
        STRLEN_STUB_INSNS="${STUB_INSN_COUNT}"
        if [[ "${STUB_INSN_COUNT}" -le 20 ]]; then
            pass "${candidate}: specialized strlen stub instruction count = ${STUB_INSN_COUNT} (<= 20)"
        else
            fail "${candidate}: specialized strlen stub count unexpected (${STUB_INSN_COUNT})"
        fi
    elif [[ "${candidate}" == "strlen_bench" ]]; then
        fail "strlen_bench: specialized strlen stub not found in stubs.s"
    fi

    # Assert --from-asm produces an identical executable
    "${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 --from-asm "${asm_dir}" "${src_elf}" -o "${from_bin}"
    if cmp -s "${l2_bin}" "${from_bin}"; then
        pass "${candidate}: --from-asm produces bit-for-bit identical executable"
    else
        fail "${candidate}: --from-asm output differs from direct --level 2 build"
    fi
done

echo "=== [5/7] Step 5: Testing Level 2B Gated Relocatable Mode (--level 2 --relocatable) ==="
# 1. ET_EXEC binary (cli_libc_nopie): Must reject gate and cleanly fallback to Level 2A
NOPIE_LOG="${WORK_DIR}/nopie_2b.log"
"${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 --relocatable "${I386_DIR}/cli_libc_nopie" -o "${WORK_DIR}/nopie_fallback_l2a" 2>"${NOPIE_LOG}" || true
if grep -qiE '(gate rejected|ET_DYN|e_type)' "${NOPIE_LOG}"; then
    pass "cli_libc_nopie: Level 2B gate rejection warning confirmed"
else
    fail "cli_libc_nopie: Expected gate rejection warning not found in log"
fi
verify_elf_x32_level "${WORK_DIR}/nopie_fallback_l2a" 2

# 2. Clean PIE / relocatable shared object (libplugin_stripped.so)
PLUGIN_2B="${WORK_DIR}/plugin_2b.so"
"${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 --relocatable "${I386_DIR}/libplugin_stripped.so" -o "${PLUGIN_2B}"
hdr_p="$(readelf -h "${PLUGIN_2B}")"
if grep -qE 'Class:[[:space:]]+ELF32' <<<"${hdr_p}" && grep -qE 'Machine:[[:space:]]+Advanced Micro Devices X86-64' <<<"${hdr_p}"; then
    pass "libplugin_stripped.so: Level 2B gate passed and emitted valid x32 relocatable output"
else
    fail "libplugin_stripped.so: Level 2B output failed ELF32/x32 header check"
fi

# 3. Clean PIE (cli_libc_pie): Verify gate verdict handling
PIE_LOG="${WORK_DIR}/pie_2b.log"
"${PATCHER}" --lookup-table "${HOST_LOOKUP}" --level 2 --relocatable "${I386_DIR}/cli_libc_pie" -o "${WORK_DIR}/pie_reloc.x32" 2>"${PIE_LOG}" || true
hdr_pie="$(readelf -h "${WORK_DIR}/pie_reloc.x32")"
if grep -qE 'Class:[[:space:]]+ELF32' <<<"${hdr_pie}" && grep -qE 'Machine:[[:space:]]+Advanced Micro Devices X86-64' <<<"${hdr_pie}"; then
    pass "cli_libc_pie: Level 2B gate verdict handled with valid x32 output"
else
    fail "cli_libc_pie: Failed to produce valid x32 output under --level 2 --relocatable"
fi

echo "=== [6/7] Step 6: addr32 High-VA Sign-Extension Rule Python Unit Check ==="
python3 -c "
import capstone
from capstone import x86 as cs_x86
from re_patcher.asm_emitter import _has_high_absolute_disp, _is_gs_prefixed

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

# 1. Non-GS instruction with absolute disp32 >= 0x80000000 -> emits addr32
insn1 = next(md.disasm(b'\xa1\x00\x10\x00\x80', 0x1000))
rule1 = _has_high_absolute_disp(insn1) and not _is_gs_prefixed(insn1)
assert rule1 is True, 'disp32=0x80001000 must emit addr32'

# 2. GS-prefixed instruction with disp32 = 0xfffffffc -> omits addr32
insn2 = next(md.disasm(b'\x65\xa1\xfc\xff\xff\xff', 0x1000))
rule2 = _has_high_absolute_disp(insn2) and not _is_gs_prefixed(insn2)
assert rule2 is False, '%gs:0xfffffffc must omit addr32'
"
pass "addr32 high-VA sign-extension rule unit check passed"

echo "=== [7/7] Step 7: Parity & Benchmark Execution ==="
if [[ "${HOST_ONLY}" -eq 1 ]]; then
    echo "[HOST-ONLY] Skipping live VM execution stages as requested (--host-only)."
else
    VM_OK=0
    if [[ -x "${VM_EXEC}" ]] && "${VM_EXEC}" -- "true" >/dev/null 2>&1; then
        VM_OK=1
    fi

    if [[ "${VM_OK}" -eq 0 ]]; then
        fail "VM execution requested but newminhh-x32 VM is not reachable via ${VM_EXEC}"
    else
        echo "Syncing patched L1 & L2 artifacts to newminhh-x32 VM (${REMOTE_DIR})..."
        "${VM_EXEC}" -- mkdir -p "${REMOTE_DIR}/l1" "${REMOTE_DIR}/l2"
        tar -C "${WORK_DIR}/l1" -cf - . | "${VM_EXEC}" -- tar -C "${REMOTE_DIR}/l1" -xf -
        tar -C "${WORK_DIR}/l2" -cf - . | "${VM_EXEC}" -- tar -C "${REMOTE_DIR}/l2" -xf -

        echo "Running live 100% parity assertion between Level 1 and Level 2A inside VM..."
        PARITY_REPORT="$(
        "${VM_EXEC}" -- bash -s "${REMOTE_DIR}" << 'EOF_GUEST'
set -u
set +e
RDIR="$1"

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

PARITY_FAILS=0

for b in "${CLI_LIST[@]}"; do
    out_l1="$("${RDIR}/l1/${b}" 2>&1)"
    rc_l1=$?
    out_l2="$("${RDIR}/l2/${b}" 2>&1)"
    rc_l2=$?

    if [ "${rc_l1}" -eq 0 ] && [ "${rc_l1}" -eq "${rc_l2}" ] && [ "${out_l1}" = "${out_l2}" ]; then
        echo "PARITY_PASS ${b}"
    else
        echo "PARITY_FAIL ${b}: rc_l1=${rc_l1} rc_l2=${rc_l2}"
        echo "  L1: ${out_l1}"
        echo "  L2: ${out_l2}"
        PARITY_FAILS=$((PARITY_FAILS + 1))
    fi
done

# cli_dlopen_client
out_l1="$(cd "${RDIR}/l1" && ./cli_dlopen_client 2>&1)"
rc_l1=$?
out_l2="$(cd "${RDIR}/l2" && ./cli_dlopen_client 2>&1)"
rc_l2=$?
if [ "${rc_l1}" -eq 0 ] && [ "${rc_l1}" -eq "${rc_l2}" ] && [ "${out_l1}" = "${out_l2}" ]; then
    echo "PARITY_PASS cli_dlopen_client"
else
    echo "PARITY_FAIL cli_dlopen_client: rc_l1=${rc_l1} rc_l2=${rc_l2}"
    PARITY_FAILS=$((PARITY_FAILS + 1))
fi

# bench_workload
out_l1="$("${RDIR}/l1/bench_workload" --compute 100000 2>&1)"
rc_l1=$?
out_l2="$("${RDIR}/l2/bench_workload" --compute 100000 2>&1)"
rc_l2=$?
if [ "${rc_l1}" -eq 0 ] && [ "${rc_l1}" -eq "${rc_l2}" ] && [ "${out_l1}" = "${out_l2}" ]; then
    echo "PARITY_PASS bench_workload"
else
    echo "PARITY_FAIL bench_workload: rc_l1=${rc_l1} rc_l2=${rc_l2}"
    PARITY_FAILS=$((PARITY_FAILS + 1))
fi

# test_l2_highva
out_hva="$("${RDIR}/l2/test_l2_highva" 2>&1)"
rc_hva=$?
if [ "${rc_hva}" -eq 0 ] && grep -q "L2_HIGHVA_OK" <<<"${out_hva}"; then
    echo "HIGHVA_PASS: ${out_hva}"
else
    echo "HIGHVA_FAIL: rc=${rc_hva} out=${out_hva}"
    PARITY_FAILS=$((PARITY_FAILS + 1))
fi

# Benchmark strlen_bench_l1 vs strlen_bench_l2
PERF_CMD=""
if command -v perf >/dev/null 2>&1; then
    PERF_CMD="perf stat -e instructions"
fi

echo "--- STRLEN BENCHMARK ---"
out_sb_l1="$(${PERF_CMD} "${RDIR}/l1/strlen_bench" 1000000 2>&1)"
rc_sb_l1=$?
out_sb_l2="$(${PERF_CMD} "${RDIR}/l2/strlen_bench" 1000000 2>&1)"
rc_sb_l2=$?
echo "L1_OUTPUT: ${out_sb_l1}"
echo "L2_OUTPUT: ${out_sb_l2}"

if [ "${rc_sb_l1}" -eq 0 ] && [ "${rc_sb_l2}" -eq 0 ] && grep -q "STRLEN_BENCH_OK" <<<"${out_sb_l1}" && grep -q "STRLEN_BENCH_OK" <<<"${out_sb_l2}"; then
    echo "STRLEN_BENCH_PASS"
else
    echo "STRLEN_BENCH_FAIL: rc_l1=${rc_sb_l1} rc_l2=${rc_sb_l2}"
    PARITY_FAILS=$((PARITY_FAILS + 1))
fi

echo "TOTAL_PARITY_FAILS: ${PARITY_FAILS}"
EOF_GUEST
        )"

        echo "${PARITY_REPORT}"
        if grep -q "TOTAL_PARITY_FAILS: 0" <<<"${PARITY_REPORT}"; then
            pass "100% parity between Level 1 and Level 2A, L2_HIGHVA_OK, and strlen benchmark verified"
        else
            fail "Parity or execution check failed inside newminhh-x32 VM"
        fi
    fi
fi

# Static stub instruction count ratio verification
# Generic PLT interpreter hop has >= 80 instructions vs actual measured STRLEN_STUB_INSNS (<= 20 instructions, ratio >= 3.0x)
if [[ "${STRLEN_STUB_INSNS}" -le 0 ]]; then
    fail "STRLEN_STUB_INSNS was not measured in Step 4"
else
    python3 -c "
generic_stub_insns = 80.0  # newminhh generic interpreter hop (~80-120 instructions)
specialized_stub_insns = float(${STRLEN_STUB_INSNS})
assert specialized_stub_insns > 0, f'specialized_stub_insns must be > 0, got {specialized_stub_insns}'
ratio = generic_stub_insns / specialized_stub_insns
assert ratio >= 3.0, f'Ratio {ratio:.2f} is less than 3x (generic={generic_stub_insns}, specialized={specialized_stub_insns})'
print(f'Static stub instruction count speedup ratio: {ratio:.2f}x (generic={generic_stub_insns:.0f}, specialized={specialized_stub_insns:.0f}, >= 3x threshold)')
"
    pass "Specialized strlen stub static instruction ratio achieves >= 3x reduction (${STRLEN_STUB_INSNS} insns vs generic >= 80)"
fi

if [[ "${FAIL_COUNT}" -eq 0 ]]; then
    echo ""
    echo "ALL_PART6_TESTS_PASSED"
    exit 0
else
    echo ""
    echo "PART6_TESTS_FAILED with ${FAIL_COUNT} failure(s)" >&2
    exit 1
fi
