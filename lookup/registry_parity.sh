#!/usr/bin/env bash
# tests/lookup/registry_parity.sh
# Validates parity between 00-libc.txt, golden_compat_191.tsv, and compat_registry.gen.h
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

GOLDEN_TSV="${WORKSPACE_ROOT}/tests/lookup/golden_compat_191.tsv"
LIBC_RULES="${WORKSPACE_ROOT}/src/lookup/rules.d/00-libc.txt"
GLIBC_ALIASES="${WORKSPACE_ROOT}/src/lookup/rules.d/10-glibc-aliases.txt"
MUSL_ALIASES="${WORKSPACE_ROOT}/src/lookup/rules.d/11-musl-aliases.txt"
COMPAT_GEN_H="${WORKSPACE_ROOT}/src/compat/compat_registry.gen.h"
LOOKUP_GEN_BIN="${WORKSPACE_ROOT}/src/lookup/newminhh-lookup-gen"

echo "=== Running newminhh Registry Parity Suite (Wave 1B) ==="

if [[ ! -f "${GOLDEN_TSV}" ]]; then
    echo "ERROR: Golden TSV not found at ${GOLDEN_TSV}" >&2
    exit 1
fi

if [[ ! -f "${LIBC_RULES}" ]]; then
    echo "ERROR: Libc rules not found at ${LIBC_RULES}" >&2
    exit 1
fi

python3 - << EOF
import os
import re
import subprocess
import sys

golden_path = "${GOLDEN_TSV}"
libc_path = "${LIBC_RULES}"
compat_gen_path = "${COMPAT_GEN_H}"
glibc_aliases_path = "${GLIBC_ALIASES}"
musl_aliases_path = "${MUSL_ALIASES}"
lookup_gen_bin = "${LOOKUP_GEN_BIN}"

# 1. Parse golden TSV
golden_entries = []
with open(golden_path, "r", encoding="utf-8") as f:
    for line_no, line in enumerate(f, 1):
        line = line.rstrip("\n\r")
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 5:
            sys.exit(f"Malformed golden TSV line {line_no}: {line}")
        name, fn, sig, flags, size = parts
        golden_entries.append((name, fn, sig, flags, int(size)))

if len(golden_entries) != 191:
    sys.exit(f"Golden TSV must contain exactly 191 entries, found {len(golden_entries)}")

# 2. Parse 00-libc.txt
libc_lib_sonames = set()
libc_runtime_syms = []
libc_static_syms = []

with open(libc_path, "r", encoding="utf-8") as f:
    first_line = f.readline().strip()
    if first_line != "#!newminhh-lookup v1":
        sys.exit(f"00-libc.txt missing required header #!newminhh-lookup v1, got: {first_line}")
    for line_no, line in enumerate(f, 2):
        line = line.rstrip("\n\r")
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        rec_type = parts[0]
        if rec_type == "lib":
            if len(parts) != 9:
                sys.exit(f"Malformed lib record line {line_no} in 00-libc.txt: {line}")
            libc_lib_sonames.add(parts[1])
        elif rec_type == "sym":
            if len(parts) != 9:
                sys.exit(f"Malformed sym record line {line_no} in 00-libc.txt: {line}")
            _, i386_soname, i386_sym, tgt_soname, tgt_sym, cls, sig, flags, size = parts
            if cls != "COMPAT":
                sys.exit(f"Expected COMPAT class in 00-libc.txt line {line_no}, got: {cls}")
            if "STATIC_ONLY" in flags:
                libc_static_syms.append((i386_sym, tgt_sym, sig, flags, int(size)))
            else:
                libc_runtime_syms.append((i386_sym, tgt_sym, sig, flags, int(size)))

# Verify libc library mappings
expected_libs = {
    "libc.so.6", "libc.so", "libm.so.6", "libm.so",
    "libpthread.so.0", "libpthread.so", "libdl.so.2", "libdl.so",
    "librt.so.1", "librt.so", "ld-linux.so.2"
}
missing_libs = expected_libs - libc_lib_sonames
if missing_libs:
    sys.exit(f"00-libc.txt is missing expected library records: {sorted(missing_libs)}")

# Sort runtime symbols for comparison
libc_runtime_syms.sort(key=lambda x: x[0])

if len(libc_runtime_syms) != 191:
    sys.exit(f"00-libc.txt runtime symbols must contain exactly 191 entries, found {len(libc_runtime_syms)}")

# 3. Check 191/191 Parity between golden TSV and 00-libc.txt
mismatches = []
for idx, (g, r) in enumerate(zip(golden_entries, libc_runtime_syms)):
    g_name, g_fn, g_sig, g_flags, g_sz = g
    r_name, r_fn, r_sig, r_flags, r_sz = r

    if g_name != r_name:
        mismatches.append(f"[{idx}] Name mismatch: golden='{g_name}' vs rules='{r_name}'")
        continue
    if g_fn != r_fn:
        mismatches.append(f"[{g_name}] Wrapper mismatch: golden='{g_fn}' vs rules='{r_fn}'")
    if g_sig != r_sig:
        mismatches.append(f"[{g_name}] Signature mismatch: golden='{g_sig}' vs rules='{r_sig}'")
    if g_sz != r_sz:
        mismatches.append(f"[{g_name}] Size mismatch: golden={g_sz} vs rules={r_sz}")

    g_flag_set = set(g_flags.split(",")) - {"-"}
    r_flag_set = set(r_flags.split(",")) - {"-", "PURE", "RUNTIME_ONLY"}
    if g_flag_set != r_flag_set:
        mismatches.append(f"[{g_name}] Flag parity mismatch: golden={g_flag_set} vs rules={r_flag_set}")

if mismatches:
    print("\nParity verification errors between 00-libc.txt and golden TSV:")
    for err in mismatches[:20]:
        print(f"  - {err}")
    if len(mismatches) > 20:
        print(f"  ... and {len(mismatches) - 20} more errors")
    sys.exit(1)

# 4. Check static-only entries (CRT weak stubs and static dlsym)
static_names = {x[0]: x for x in libc_static_syms}
for crt_sym in ["__gmon_start__", "_ITM_registerTMCloneTable", "_ITM_deregisterTMCloneTable", "_Jv_RegisterClasses"]:
    if crt_sym not in static_names:
        sys.exit(f"Missing weak CRT static stub in 00-libc.txt: {crt_sym}")
    entry = static_names[crt_sym]
    if "WEAK" not in entry[3] or "STATIC_ONLY" not in entry[3]:
        sys.exit(f"Weak CRT stub {crt_sym} missing WEAK,STATIC_ONLY: {entry[3]}")

if "dlsym" not in static_names:
    sys.exit("Missing static dlsym in 00-libc.txt")
dlsym_static = static_names["dlsym"]
if dlsym_static[1] != "compat_dlsym_cdecl" or "STATIC_ONLY" not in dlsym_static[3] or "CDECL_DIRECT" in dlsym_static[3]:
    sys.exit(f"Invalid static dlsym entry in 00-libc.txt: {dlsym_static}")

# 5. Check compat_registry.gen.h if it exists
if os.path.exists(compat_gen_path):
    with open(compat_gen_path, "r", encoding="utf-8") as f:
        gen_content = f.read()
    gen_matches = re.findall(r'\{\s*"([^"]+)"\s*,\s*(?:&|\(void \*\)\(uintptr_t\))?([a-zA-Z0-9_]+)', gen_content)
    if len(gen_matches) != 191:
        sys.exit(f"compat_registry.gen.h has {len(gen_matches)} entries, expected 191")
    for (g_name, g_fn, _, _, _), (c_name, c_fn) in zip(golden_entries, gen_matches):
        if g_name != c_name or g_fn != c_fn:
            sys.exit(f"compat_registry.gen.h mismatch: golden=({g_name},{g_fn}) vs gen=({c_name},{c_fn})")
    print(f"Verified compat_registry.gen.h parity: 191/191 matching symbols.")

# 6. Check newminhh-lookup-gen --emit-c-registry if binary exists
if os.path.exists(lookup_gen_bin):
    rules_dir = os.path.dirname(libc_path)
    tmp_out = "/tmp/_test_gen_parity.h"
    res = subprocess.run([lookup_gen_bin, "--emit-c-registry", rules_dir, "-o", tmp_out, "--quiet"], capture_output=True)
    if res.returncode != 0:
        sys.exit(f"newminhh-lookup-gen failed: {res.stderr.decode()}")
    with open(tmp_out, "r", encoding="utf-8") as f:
        dynamic_gen = f.read()
    dyn_matches = re.findall(r'\{\s*"([^"]+)"\s*,\s*(?:&|\(void \*\)\(uintptr_t\))?([a-zA-Z0-9_]+)', dynamic_gen)
    if len(dyn_matches) != 191:
        sys.exit(f"newminhh-lookup-gen emitted {len(dyn_matches)} entries, expected 191")
    for (g_name, g_fn, _, _, _), (d_name, d_fn) in zip(golden_entries, dyn_matches):
        if g_name != d_name or g_fn != d_fn:
            sys.exit(f"newminhh-lookup-gen mismatch: golden=({g_name},{g_fn}) vs gen=({d_name},{d_fn})")
    print(f"Verified newminhh-lookup-gen --emit-c-registry parity: 191/191 matching symbols.")

# 7. Check ZERO overlap with 10-glibc-aliases.txt and 11-musl-aliases.txt
all_libc_syms = {x[0] for x in libc_runtime_syms} | {x[0] for x in libc_static_syms}

for alias_file in [glibc_aliases_path, musl_aliases_path]:
    if os.path.exists(alias_file):
        with open(alias_file, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if parts[0] == "sym":
                    sym_name = parts[2]
                    if sym_name in all_libc_syms:
                        sys.exit(f"OVERLAP ERROR: Symbol '{sym_name}' in {os.path.basename(alias_file)} overlaps with 00-libc.txt!")
        print(f"Verified ZERO overlap between {os.path.basename(alias_file)} and 00-libc.txt.")

print("191/191 PASS: Full parity achieved across golden registry, 00-libc.txt, and aliases!")
EOF

echo "All registry parity checks passed successfully!"
