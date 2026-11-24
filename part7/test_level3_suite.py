#!/usr/bin/env python3
"""Part 7: End-to-end Level 3 (3A gated boost + 3C assist) verification suite."""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Dict, List, Set, Tuple

from elftools.elf.elffile import ELFFile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PATCHER_SRC = os.path.join(REPO_ROOT, "src", "patcher")
if PATCHER_SRC not in sys.path:
    sys.path.insert(0, PATCHER_SRC)

from re_patcher.level3 import (  # noqa: E402
    BoostedFunction,
    FunctionSignature,
    IDAProBackend,
    Level3BoostReport,
    RzGhidraBackend,
    can_run_i386_native,
    can_run_x32_native,
    compile_freestanding_x32,
    evaluate_purity_gate,
    generate_assist_scaffold,
    generate_cdecl_to_x32_boundary_stub,
    run_level3_boost,
    sanitize_function,
    verify_boosted_function,
)

CORPUS_I386 = os.path.join(REPO_ROOT, "tests", "corpus", "build", "i386")


def _load_elf_functions(
    elf_path: str,
) -> Tuple[List[Tuple[int, int]], List[Tuple[int, str, bytes]]]:
    """Extract PT_LOAD segment ranges and defined STT_FUNC bytes from an ELF32 binary."""
    with open(elf_path, "rb") as fh:
        elf = ELFFile(fh)
        seg_ranges = [
            (int(seg["p_vaddr"]), int(seg["p_vaddr"]) + int(seg["p_memsz"]))
            for seg in elf.iter_segments()
            if seg["p_type"] == "PT_LOAD"
        ]
        funcs_by_va: Dict[int, Tuple[int, str, bytes]] = {}
        for secname in (".symtab", ".dynsym"):
            sec = elf.get_section_by_name(secname)
            if sec is None:
                continue
            for sym in sec.iter_symbols():
                if (
                    sym["st_info"]["type"] == "STT_FUNC"
                    and sym["st_shndx"] != "SHN_UNDEF"
                    and int(sym["st_size"]) > 0
                ):
                    va = int(sym["st_value"])
                    size = int(sym["st_size"])
                    if va in funcs_by_va:
                        continue
                    offsets = list(elf.address_offsets(va, size))
                    if not offsets:
                        continue
                    fh.seek(offsets[0])
                    code = fh.read(size)
                    if len(code) == size:
                        funcs_by_va[va] = (va, sym.name, code)

    funcs = [funcs_by_va[va] for va in sorted(funcs_by_va)]
    return seg_ranges, funcs


def _find_outcome(report: Level3BoostReport, name: str):
    for out in report.outcomes:
        if out.name == name:
            return out
    raise AssertionError(f"Function '{name}' not found in outcomes: {[o.name for o in report.outcomes]}")


def _list_ida_artifacts(directory: str) -> Set[str]:
    exts = (".i64", ".idb", ".id0", ".id1", ".id2", ".nam", ".til")
    found: Set[str] = set()
    for root, _dirs, files in os.walk(directory):
        for fname in files:
            if fname.lower().endswith(exts):
                found.add(os.path.join(root, fname))
    return found


def test_1_libplugin_rz_ghidra_boost() -> Level3BoostReport:
    print("[Test 1] Gate + full run_level3_boost via rz-ghidra on libplugin_stripped.so")
    rz_cap = RzGhidraBackend().probe()
    assert rz_cap.available, f"rz-ghidra must be available on host: {rz_cap.reason}"

    elf_path = os.path.join(CORPUS_I386, "libplugin_stripped.so")
    seg_ranges, funcs = _load_elf_functions(elf_path)

    report = run_level3_boost(
        elf_path,
        funcs,
        seg_ranges=seg_ranges,
        known_imports={},
        decompiler_choice="rz-ghidra",
        trials=4096,
    )
    assert report.backend == "rz-ghidra", report.backend
    assert report.total_functions == len(funcs)

    # plugin_name MUST be rejected at gate
    out_name = _find_outcome(report, "plugin_name")
    assert not out_name.boosted, f"plugin_name must NOT be boosted: {out_name}"
    assert out_name.stage_failed == "gate", f"expected gate failure, got {out_name}"

    # Verify sanitizer ALSO rejects synthetic `return 0x2000;` inside PT_LOAD
    synth_res = sanitize_function(
        "char *plugin_name(void) { return (char *)0x2000; }",
        func_name="plugin_name",
        expected_signature=FunctionSignature("i32", []),
        segment_ranges=[(0x2000, 0x3000)],
    )
    assert not synth_res.ok, "sanitizer must reject return 0x2000; inside PT_LOAD"
    assert "0x2000" in (synth_res.reason or ""), synth_res.reason

    # plugin_compute MUST be boosted and verified (4096/4096)
    out_comp = _find_outcome(report, "plugin_compute")
    assert out_comp.boosted and out_comp.boosted_func is not None, f"plugin_compute failed: {out_comp}"
    bf = out_comp.boosted_func
    assert bf.trials_run == 4096, bf.trials_run
    assert bf.ret_type == "i32" and bf.arg_types == ["i32", "i32"], (bf.ret_type, bf.arg_types)
    assert len(bf.text_bytes) > 0
    assert "__nm_l3_entry_" in bf.boundary_stub_asm
    assert "nm_l3_plugin_compute" in bf.boundary_stub_asm

    # Verify clean `as --x32` + `ld -m elf32_x86_64` splice on BoostedFunction bytes
    with tempfile.TemporaryDirectory(prefix="part7_splice_") as td:
        stub_s = os.path.join(td, "stub.s")
        stub_o = os.path.join(td, "stub.o")
        comp_o = os.path.join(td, "comp.o")
        spliced = os.path.join(td, "spliced.elf")
        with open(stub_s, "w", encoding="utf-8") as fh:
            fh.write(bf.boundary_stub_asm)
        with open(comp_o, "wb") as fh:
            fh.write(bf.obj_bytes)
        p_as = subprocess.run(["as", "--x32", stub_s, "-o", stub_o], capture_output=True, text=True, check=False)
        assert p_as.returncode == 0, p_as.stderr
        p_ld = subprocess.run(
            ["ld", "-m", "elf32_x86_64", "-e", f"__nm_l3_entry_{bf.va:x}", stub_o, comp_o, "-o", spliced],
            capture_output=True,
            text=True,
            check=False,
        )
        assert p_ld.returncode == 0 and os.path.isfile(spliced), p_ld.stderr

    # Live x32 ABI boundary stub execution check if host executes x32 natively
    if can_run_x32_native():
        _verify_boundary_stub_runtime(bf)

    print(
        f"  [PASS] libplugin_stripped.so (rz-ghidra): boosted={report.boosted_count}/{report.total_functions}, "
        f"plugin_compute verified {bf.trials_run}/{bf.trials_run} ({bf.diff_mode})"
    )
    return report


def _verify_boundary_stub_runtime(bf: BoostedFunction) -> None:
    """Execute __nm_l3_entry_<va> in a live freestanding x32 binary and verify ABI preservation."""
    driver_c = rf"""
__asm__(
".globl _start\n"
"_start:\n"
"    call test_main\n"
"    mov %eax, %edi\n"
"    mov $0x400000E7, %rax\n"
"    syscall\n"
"    hlt\n"
);

extern void __nm_l3_entry_{bf.va:x}(void);

static int call_guest_cdecl_2(int a, int b) {{
    int ret_val = 0;
    unsigned long long ok = 1;
    __asm__ volatile(
        "mov %%rsp, %%r12\n"
        "leaq -4(%%rsp), %%rsp\n"
        "movl %[b], (%%rsp)\n"
        "leaq -4(%%rsp), %%rsp\n"
        "movl %[a], (%%rsp)\n"
        "mov $0x1122334455667788, %%rsi\n"
        "mov $0x99aabbccddeeff00, %%rdi\n"
        "leal 1f(%%rip), %%r11d\n"
        "leaq -4(%%rsp), %%rsp\n"
        "movl %%r11d, (%%rsp)\n"
        "jmp __nm_l3_entry_{bf.va:x}\n"
        "1:\n"
        "leaq 8(%%rsp), %%rsp\n"
        "cmp %%r12, %%rsp\n"
        "je 2f\n"
        "xor %%eax, %%eax\n"
        "mov $0, %[ok]\n"
        "2:\n"
        "mov $0x1122334455667788, %%r13\n"
        "cmp %%r13, %%rsi\n"
        "je 3f\n"
        "mov $0, %[ok]\n"
        "3:\n"
        "mov $0x99aabbccddeeff00, %%r13\n"
        "cmp %%r13, %%rdi\n"
        "je 4f\n"
        "mov $0, %[ok]\n"
        "4:\n"
        : "=a"(ret_val), [ok] "+r"(ok)
        : [a] "r"(a), [b] "r"(b)
        : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "memory", "cc"
    );
    if (!ok) return -999999;
    return ret_val;
}}

int test_main(void) {{
    int r = call_guest_cdecl_2(7, 5);
    return (r == 47) ? 0 : 1;
}}
"""
    with tempfile.TemporaryDirectory(prefix="part7_stub_exec_") as td:
        stub_s = os.path.join(td, "stub.s")
        stub_o = os.path.join(td, "stub.o")
        comp_o = os.path.join(td, "comp.o")
        drv_c = os.path.join(td, "drv.c")
        exe = os.path.join(td, "drv_x32")
        with open(stub_s, "w", encoding="utf-8") as fh:
            fh.write(bf.boundary_stub_asm)
        with open(comp_o, "wb") as fh:
            fh.write(bf.obj_bytes)
        with open(drv_c, "w", encoding="utf-8") as fh:
            fh.write(driver_c)
        assert subprocess.run(["as", "--x32", stub_s, "-o", stub_o], check=False).returncode == 0
        p_cc = subprocess.run(
            [
                "gcc", "-mx32", "-ffreestanding", "-fno-stack-protector",
                "-fcf-protection=none", "-nostdlib", "-static", "-no-pie", "-O2",
                drv_c, stub_o, comp_o, "-o", exe,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        assert p_cc.returncode == 0, p_cc.stderr
        p_run = subprocess.run([exe], check=False)
        assert p_run.returncode == 0, f"boundary stub live execution failed rc={p_run.returncode}"


def test_2_corpus_rejections_and_counts() -> Dict[str, Level3BoostReport]:
    print("[Test 2] Gate + run_level3_boost on libbasic_pic.so, bench_workload, cli_structs_abi")
    reports: Dict[str, Level3BoostReport] = {}

    # libbasic_pic.so
    basic_path = os.path.join(CORPUS_I386, "libbasic_pic.so")
    seg_b, funcs_b = _load_elf_functions(basic_path)
    rep_b = run_level3_boost(
        basic_path, funcs_b, seg_ranges=seg_b, known_imports={}, decompiler_choice="rz-ghidra", trials=512
    )
    reports["libbasic_pic.so"] = rep_b

    out_switch = _find_outcome(rep_b, "switch_step")
    assert not out_switch.boosted
    assert out_switch.stage_failed == "gate"
    assert any("regparm-entry-read" in r for r in out_switch.gate_reasons), out_switch.gate_reasons

    out_bam = _find_outcome(rep_b, "basic_add_mul")
    assert not out_bam.boosted
    assert out_bam.stage_failed == "gate"
    assert any("call" in r for r in out_bam.gate_reasons), out_bam.gate_reasons

    # bench_workload
    bench_path = os.path.join(CORPUS_I386, "bench_workload")
    seg_w, funcs_w = _load_elf_functions(bench_path)
    rep_w = run_level3_boost(
        bench_path, funcs_w, seg_ranges=seg_w, known_imports={}, decompiler_choice="rz-ghidra", trials=512
    )
    reports["bench_workload"] = rep_w

    out_cstep = _find_outcome(rep_w, "compute_step")
    assert not out_cstep.boosted
    assert out_cstep.stage_failed == "gate"
    assert any("regparm-entry-read" in r for r in out_cstep.gate_reasons), out_cstep.gate_reasons

    # cli_structs_abi
    structs_path = os.path.join(CORPUS_I386, "cli_structs_abi")
    seg_s, funcs_s = _load_elf_functions(structs_path)
    rep_s = run_level3_boost(
        structs_path, funcs_s, seg_ranges=seg_s, known_imports={}, decompiler_choice="rz-ghidra", trials=512
    )
    reports["cli_structs_abi"] = rep_s

    for bname, rep in reports.items():
        print(
            f"  [PASS] {bname}: total={rep.total_functions}, "
            f"gate_passed={rep.gate_passed}, boosted={rep.boosted_count}"
        )
    return reports


def test_3_injected_bug_and_safety_checks() -> None:
    print("[Test 3] Injected-bug diffverify rejection + compile safety + i64/f64 boundary stubs")
    elf_path = os.path.join(CORPUS_I386, "libplugin_stripped.so")
    _seg_ranges, funcs = _load_elf_functions(elf_path)
    comp_entry = next((va, name, code) for va, name, code in funcs if name == "plugin_compute")
    va, name, orig_bytes = comp_entry

    # Corrupt boosted C: + 13 instead of + 12
    buggy_c = (
        "#include <stdint.h>\n"
        "int32_t nm_l3_plugin_compute(int32_t a, int32_t b) {\n"
        "    return (a * b) + 13;\n"
        "}\n"
    )
    obj_path, text_bytes, err = compile_freestanding_x32(buggy_c, name)
    assert obj_path is not None and text_bytes is not None, f"unexpected compile err: {err}"
    try:
        dv = verify_boosted_function(
            name, va, orig_bytes, obj_path, "i32", ["i32", "i32"], trials=256
        )
        assert not dv.passed, "diffverify MUST catch +13 vs +12 mismatch!"
        assert dv.first_mismatch is not None and "trial 0" in dv.first_mismatch, dv
    finally:
        shutil.rmtree(os.path.dirname(obj_path), ignore_errors=True)

    # Reject unexpected .rodata (string literal) in compile_freestanding_x32
    rodata_c = (
        "#include <stdint.h>\n"
        "const char *nm_l3_bad_str(void) { return \"hello_rodata\"; }\n"
    )
    bad_obj, _bad_txt, bad_err = compile_freestanding_x32(rodata_c, "bad_str")
    if bad_obj is not None:
        shutil.rmtree(os.path.dirname(bad_obj), ignore_errors=True)
        raise AssertionError("compile_freestanding_x32 must reject non-empty .rodata")
    assert ".rodata" in bad_err or "unexpected" in bad_err, bad_err

    # Verify i64 and f64 boundary stubs assemble and execute cleanly on x32
    if can_run_x32_native():
        i64_c = (
            "#include <stdint.h>\n"
            "int64_t nm_l3_add64(int64_t a, int32_t b) { return a + (int64_t)b; }\n"
        )
        f64_c = (
            "#include <stdint.h>\n"
            "double nm_l3_addf(double a, float b) { return a + (double)b; }\n"
        )
        o64, _, e64 = compile_freestanding_x32(i64_c, "add64")
        of64, _, ef64 = compile_freestanding_x32(f64_c, "addf")
        assert o64 is not None, e64
        assert of64 is not None, ef64
        try:
            stub64 = generate_cdecl_to_x32_boundary_stub("add64", 0x2000, "i64", ["i64", "i32"])
            stubf = generate_cdecl_to_x32_boundary_stub("addf", 0x2100, "f64", ["f64", "f32"])
            with tempfile.TemporaryDirectory(prefix="part7_abi_") as td:
                s64_s = os.path.join(td, "s64.s")
                s64_o = os.path.join(td, "s64.o")
                sf_s = os.path.join(td, "sf.s")
                sf_o = os.path.join(td, "sf.o")
                drv_c = os.path.join(td, "drv.c")
                exe = os.path.join(td, "drv")
                open(s64_s, "w", encoding="utf-8").write(stub64)
                open(sf_s, "w", encoding="utf-8").write(stubf)
                assert subprocess.run(["as", "--x32", s64_s, "-o", s64_o], check=False).returncode == 0
                assert subprocess.run(["as", "--x32", sf_s, "-o", sf_o], check=False).returncode == 0
                open(drv_c, "w", encoding="utf-8").write(r"""
__asm__(
".globl _start\n"
"_start:\n"
"    call test_abi\n"
"    mov %eax, %edi\n"
"    mov $0x400000E7, %rax\n"
"    syscall\n"
"    hlt\n"
);
extern void __nm_l3_entry_2000(void);
extern void __nm_l3_entry_2100(void);
int test_abi(void) {
    unsigned int lo = 0, hi = 0;
    unsigned long long a64 = 0x100000001ULL;
    int b32 = 41;
    __asm__ volatile(
        "leaq -4(%%rsp), %%rsp\n"
        "movl %[b], (%%rsp)\n"
        "leaq -8(%%rsp), %%rsp\n"
        "movq %[a], (%%rsp)\n"
        "leal 1f(%%rip), %%r11d\n"
        "leaq -4(%%rsp), %%rsp\n"
        "movl %%r11d, (%%rsp)\n"
        "jmp __nm_l3_entry_2000\n"
        "1:\n"
        "leaq 12(%%rsp), %%rsp\n"
        : "=a"(lo), "=d"(hi)
        : [a] "r"(a64), [b] "r"(b32)
        : "rcx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc"
    );
    if (lo != 42U || hi != 1U) return 1;

    double da = 2.5, fout = 0.0;
    float fb = 1.25f;
    double *pout = &fout;
    __asm__ volatile(
        "leaq -4(%%rsp), %%rsp\n"
        "movss %[fb], (%%rsp)\n"
        "leaq -8(%%rsp), %%rsp\n"
        "movsd %[da], (%%rsp)\n"
        "leal 1f(%%rip), %%r11d\n"
        "leaq -4(%%rsp), %%rsp\n"
        "movl %%r11d, (%%rsp)\n"
        "jmp __nm_l3_entry_2100\n"
        "1:\n"
        "leaq 12(%%rsp), %%rsp\n"
        "fstpl (%%rbx)\n"
        :
        : [da] "x"(da), [fb] "x"(fb), "b"(pout)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "memory", "cc"
    );
    if (fout != 3.75) return 2;
    return 0;
}
""")
                p_cc = subprocess.run(
                    [
                        "gcc", "-mx32", "-ffreestanding", "-fno-stack-protector",
                        "-fcf-protection=none", "-nostdlib", "-static", "-no-pie", "-O2",
                        drv_c, s64_o, o64, sf_o, of64, "-o", exe,
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                assert p_cc.returncode == 0, p_cc.stderr
                p_run = subprocess.run([exe], check=False)
                assert p_run.returncode == 0, f"i64/f64 boundary stub test failed rc={p_run.returncode}"
        finally:
            shutil.rmtree(os.path.dirname(o64), ignore_errors=True)
            shutil.rmtree(os.path.dirname(of64), ignore_errors=True)

    print("  [PASS] Injected-bug (+13 vs +12), .rodata rejection, and i64/f64 boundary stubs passed")


def test_4_decompile_assist_scaffold() -> None:
    print("[Test 4] --decompile-assist scaffold on libplugin_stripped.so + IDA Free manual mode")
    elf_path = os.path.join(CORPUS_I386, "libplugin_stripped.so")
    _seg_ranges, funcs = _load_elf_functions(elf_path)
    assist_funcs = [(va, name, ["sample-gate-reason"] if name == "plugin_name" else []) for va, name, _ in funcs]
    sample_imports = [
        {"symbol": "stat", "status": "FORBIDDEN_ABI", "dsl": "ipp", "lib": "libc.so.6", "reason": "struct stat layout mismatch"},
        {"symbol": "custom_untyped", "status": "UNTYPED", "dsl": "", "lib": "libfoo.so"},
        {"symbol": "sin", "status": "NATIVE_TYPED", "dsl": "dd", "lib": "libm.so.6"},
    ]

    with tempfile.TemporaryDirectory(prefix="part7_assist_") as td:
        out_rz = os.path.join(td, "assist_rz")
        rep_rz = generate_assist_scaffold(
            elf_path,
            out_rz,
            functions=assist_funcs,
            imports=sample_imports,
            decompiler_choice="rz-ghidra",
            target_interp="/lib/ld-linux-x32.so.2",
        )
        assert rep_rz.backend_name == "rz-ghidra", rep_rz
        assert not rep_rz.ida_free_manual
        for rel in ("README.md", "include/nm_imports.h", "abi_hazards.txt", "PORTING_PROMPT.md", "Makefile"):
            assert os.path.isfile(os.path.join(out_rz, rel)), f"Missing {rel}"
        assert any(os.path.basename(p) == "plugin_compute.c" for p in rep_rz.files_written)
        with open(os.path.join(out_rz, "abi_hazards.txt"), "r", encoding="utf-8") as fh:
            haz_txt = fh.read()
        assert "FORBIDDEN_ABI" in haz_txt and "UNTYPED" in haz_txt

        # IDA Free manual mode (simulate IDAFREEDIR without GUI) + --import-decompiled ingestion
        fake_ida_free = os.path.join(td, "fake_idafree")
        os.makedirs(fake_ida_free, exist_ok=True)
        out_idafree = os.path.join(td, "assist_idafree")
        old_env = os.environ.get("IDAFREEDIR")
        os.environ["IDAFREEDIR"] = fake_ida_free
        try:
            rep_free = generate_assist_scaffold(
                elf_path,
                out_idafree,
                functions=assist_funcs,
                imports=sample_imports,
                decompiler_choice="idafree",
                target_interp="/lib/ld-linux-x32.so.2",
            )
        finally:
            if old_env is None:
                os.environ.pop("IDAFREEDIR", None)
            else:
                os.environ["IDAFREEDIR"] = old_env

        assert rep_free.ida_free_manual, "Expected ida_free_manual=True"
        assert os.path.isfile(os.path.join(out_idafree, "scripts", "ida_export_c.py"))
        with open(os.path.join(out_idafree, "README.md"), "r", encoding="utf-8") as fh:
            readme_txt = fh.read()
        assert "scripts/ida_export_c.py" in readme_txt

        # Test --import-decompiled ingestion
        exp_dir = os.path.join(td, "exported_c")
        os.makedirs(exp_dir, exist_ok=True)
        comp_va = next(va for va, name, _ in funcs if name == "plugin_compute")
        with open(os.path.join(exp_dir, "plugin_compute.c"), "w", encoding="utf-8") as fh:
            fh.write("int plugin_compute(int a, int b) { return a * b + 12; }\n")
        with open(os.path.join(exp_dir, "manifest.json"), "w", encoding="utf-8") as fh:
            json.dump(
                {"functions": [{"name": "plugin_compute", "va": comp_va, "file": "plugin_compute.c"}]},
                fh,
            )
        out_ingested = os.path.join(td, "assist_ingested")
        rep_ing = generate_assist_scaffold(
            elf_path,
            out_ingested,
            functions=assist_funcs,
            imports=sample_imports,
            decompiler_choice="idafree",
            import_decompiled_dir=exp_dir,
        )
        with open(os.path.join(out_ingested, "src", "plugin_compute.c"), "r", encoding="utf-8") as fh:
            ing_src = fh.read()
        assert "return a * b + 12;" in ing_src, ing_src
        assert rep_ing.functions_exported == len(assist_funcs)

    print("  [PASS] Decompile-assist scaffold (rz-ghidra, idafree manual + scripts/ida_export_c.py, import_decompiled)")


def test_5_optional_idapro_smoke() -> str:
    print("[Test 5] Optional IDA Pro (idalib) smoke test + zero workspace .i64/.idb check")
    cap = IDAProBackend().probe()
    if not cap.available:
        print(f"  [SKIP] IDA Pro unavailable ({cap.reason})")
        return f"SKIP ({cap.reason})"

    before_artifacts = _list_ida_artifacts(REPO_ROOT)
    elf_path = os.path.join(CORPUS_I386, "libplugin_stripped.so")
    seg_ranges, funcs = _load_elf_functions(elf_path)

    rep = run_level3_boost(
        elf_path,
        funcs,
        seg_ranges=seg_ranges,
        known_imports={},
        decompiler_choice="idapro",
        trials=4096,
    )
    after_artifacts = _list_ida_artifacts(REPO_ROOT)
    leaked = sorted(after_artifacts - before_artifacts)
    assert not leaked, f"IDA Pro leaked database artifacts into workspace: {leaked}"

    assert rep.backend == "idapro", rep.backend
    out_name = _find_outcome(rep, "plugin_name")
    assert not out_name.boosted and out_name.stage_failed == "gate", out_name
    out_comp = _find_outcome(rep, "plugin_compute")
    assert out_comp.boosted and out_comp.boosted_func is not None, out_comp
    assert out_comp.boosted_func.trials_run == 4096

    print(
        f"  [PASS] IDA Pro ({cap.path}): boosted={rep.boosted_count}/{rep.total_functions}, "
        f"0 leaked .i64/.idb artifacts"
    )
    return f"PASS (path={cap.path}, boosted={rep.boosted_count}/{rep.total_functions})"


def main() -> int:
    print(f"[Part 7] Host execution tiers: i386={can_run_i386_native()}, x32={can_run_x32_native()}")
    rep1 = test_1_libplugin_rz_ghidra_boost()
    rep2 = test_2_corpus_rejections_and_counts()
    test_3_injected_bug_and_safety_checks()
    test_4_decompile_assist_scaffold()
    ida_status = test_5_optional_idapro_smoke()

    print("\n=== Part 7 Summary ===")
    print(f"  libplugin_stripped.so (rz-ghidra): boosted {rep1.boosted_count}/{rep1.total_functions}")
    for bname, rep in rep2.items():
        print(f"  {bname} (rz-ghidra): boosted {rep.boosted_count}/{rep.total_functions} (gate_passed={rep.gate_passed})")
    print(f"  IDA Pro smoke test: {ida_status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
