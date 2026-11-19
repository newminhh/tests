#!/usr/bin/env python3
"""
Part 5 Automated /proc/<pid>/smaps, RSS/PSS Memory Sharing & Execution Timing Benchmark.

Runs natively inside the Debian x32 environment (Python 3 stdlib only).
Compares:
  1. native_x32:     Native x32 bench_workload baseline
  2. patched_compat: Statically patched i386 -> x32 bench_workload (via newminhh-patch)
  3. patched_hybrid: Optional --hybrid-fallback patched bench_workload (if provided)
  4. dbt_runtime:    Unpatched i386 bench_workload via /usr/local/bin/newminhh-run
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional, Tuple


SMAPS_HDR_RE = re.compile(
    r"^([0-9a-fA-F]+)-([0-9a-fA-F]+)\s+([rwxps-]{4})\s+"
    r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+:[0-9a-fA-F]+)\s+(\d+)(?:\s+(.*))?$"
)
SMAPS_KV_RE = re.compile(r"^([A-Za-z0-9_]+):\s+(\d+)\s+kB$")

FORBIDDEN_I386_LIB_PATTERNS = (
    "/lib/i386-linux-gnu",
    "/usr/lib/i386-linux-gnu",
    "/lib32/",
    "/usr/lib32/",
    "i386-linux-gnu",
)
FORBIDDEN_I386_BASENAMES = {
    "ld-linux.so.2",
    "ld-linux.so.3",
}


class BenchFailure(RuntimeError):
    """Raised when any correctness, isolation, or memory-sharing invariant fails."""


def default_repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def parse_smaps(smaps_text: str) -> List[Dict[str, Any]]:
    vmas: List[Dict[str, Any]] = []
    cur: Optional[Dict[str, Any]] = None
    for raw_line in smaps_text.splitlines():
        line = raw_line.rstrip("\n")
        hm = SMAPS_HDR_RE.match(line)
        if hm:
            if cur is not None:
                vmas.append(cur)
            cur = {
                "start": hm.group(1),
                "end": hm.group(2),
                "perms": hm.group(3),
                "offset": hm.group(4),
                "dev": hm.group(5),
                "inode": int(hm.group(6)),
                "path": (hm.group(7) or "").strip(),
                "metrics": {},
            }
            continue
        if cur is not None:
            km = SMAPS_KV_RE.match(line)
            if km:
                cur["metrics"][km.group(1)] = int(km.group(2))
    if cur is not None:
        vmas.append(cur)
    return vmas


def check_no_i386_libs(pid: int, vmas: List[Dict[str, Any]], maps_text: str) -> None:
    paths_to_check = [v["path"] for v in vmas if v["path"]]
    for line in maps_text.splitlines():
        parts = line.split(None, 5)
        if len(parts) == 6:
            paths_to_check.append(parts[5].strip())

    for path in paths_to_check:
        if path.startswith("["):
            continue
        base = os.path.basename(path.split(" ")[0])
        if base in FORBIDDEN_I386_BASENAMES:
            raise BenchFailure(f"PID {pid} mapped forbidden i386 dynamic linker: {path}")
        for pat in FORBIDDEN_I386_LIB_PATTERNS:
            if pat in path:
                raise BenchFailure(f"PID {pid} mapped forbidden i386 library path: {path}")


def inspect_shared_library(
    pid: int,
    vmas: List[Dict[str, Any]],
    lib_basename: str,
    require_x32_dir: bool = False,
) -> Dict[str, Any]:
    matches = [
        v for v in vmas if os.path.basename(v["path"].split(" ")[0]) == lib_basename
    ]
    if not matches:
        raise BenchFailure(f"PID {pid}: expected mapping for '{lib_basename}' not found in /proc/{pid}/smaps")

    paths = sorted({v["path"].split(" ")[0] for v in matches})
    if require_x32_dir:
        if not any(("x86_64-linux-gnux32" in p or "/libx32" in p) for p in paths):
            raise BenchFailure(
                f"PID {pid}: '{lib_basename}' is not from x32 library directory: {paths}"
            )

    ro_rx_vmas = [v for v in matches if v["perms"] in ("r--p", "r-xp")]
    r_exec = [v for v in ro_rx_vmas if v["perms"] == "r-xp"]
    r_only_all = [v for v in ro_rx_vmas if v["perms"] == "r--p"]
    # Dynamic linker GNU RELRO starts as rw-p for relocation and is mprotect()'d to r--p
    # (exhibiting Anonymous > 0 and Private_Dirty == Anonymous). File-backed shared r--p
    # (.rodata / headers / dynsym) has Anonymous == 0 and Private_Dirty == 0.
    r_only_shared = [v for v in r_only_all if v["metrics"].get("Anonymous", 0) == 0]
    r_only_relro = [v for v in r_only_all if v["metrics"].get("Anonymous", 0) > 0]

    if not r_only_shared or not r_exec:
        raise BenchFailure(
            f"PID {pid}: '{lib_basename}' missing shared r--p ({len(r_only_shared)}) or r-xp ({len(r_exec)}) VMA"
        )

    shared_vmas = r_only_shared + r_exec
    inodes = {v["inode"] for v in shared_vmas}
    if len(inodes) != 1 or 0 in inodes:
        raise BenchFailure(
            f"PID {pid}: '{lib_basename}' has unexpected inodes on shared r--p/r-xp VMAs: {sorted(inodes)}"
        )
    inode = next(iter(inodes))

    for v in shared_vmas:
        pd = v["metrics"].get("Private_Dirty", 0)
        if pd != 0:
            raise BenchFailure(
                f"PID {pid}: '{lib_basename}' ({v['perms']} {v['start']}-{v['end']}) "
                f"has non-zero Private_Dirty={pd} kB"
            )

    for v in r_only_relro:
        # Ensure RELRO VMA is genuinely tiny/RELRO-only (not text/rodata COW)
        if v["offset"] == "00000000" or v["metrics"].get("Size", 0) > 64:
            raise BenchFailure(
                f"PID {pid}: '{lib_basename}' unexpected anonymous r--p VMA "
                f"({v['start']}-{v['end']} off={v['offset']} size={v['metrics'].get('Size', 0)} kB)"
            )

    ro_sc = sum(v["metrics"].get("Shared_Clean", 0) for v in r_only_shared)
    rx_sc = sum(v["metrics"].get("Shared_Clean", 0) for v in r_exec)
    total_sc = ro_sc + rx_sc
    total_rss = sum(v["metrics"].get("Rss", 0) for v in shared_vmas)
    total_pss = sum(v["metrics"].get("Pss", 0) for v in shared_vmas)
    relro_pd = sum(v["metrics"].get("Private_Dirty", 0) for v in r_only_relro)

    if ro_sc <= 0 or rx_sc <= 0:
        raise BenchFailure(
            f"PID {pid}: '{lib_basename}' expected Shared_Clean > 0 kB on both r--p ({ro_sc} kB) "
            f"and r-xp ({rx_sc} kB) VMAs"
        )

    return {
        "paths": paths,
        "inode": inode,
        "ro_shared_clean_kb": ro_sc,
        "rx_shared_clean_kb": rx_sc,
        "shared_clean_kb": total_sc,
        "private_dirty_kb": 0,
        "relro_private_dirty_kb": relro_pd,
        "rss_kb": total_rss,
        "pss_kb": total_pss,
    }


def measure_concurrent_daemons(
    mode_name: str,
    cmd_prefix: List[str],
    k_daemons: int,
    daemon_sec: int,
    require_compat: bool,
    require_dbt: bool,
) -> Dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix=f"newminhh_smaps_{mode_name}_") as tmpdir:
        procs: List[subprocess.Popen[str]] = []
        markers: List[str] = []
        for idx in range(k_daemons):
            marker = os.path.join(tmpdir, f"ready_{idx}.marker")
            markers.append(marker)
            cmd = cmd_prefix + ["--daemon", str(daemon_sec), marker]
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            procs.append(p)

        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            for idx, p in enumerate(procs):
                rc = p.poll()
                if rc is not None:
                    out, err = p.communicate()
                    for other in procs:
                        if other.poll() is None:
                            other.kill()
                    raise BenchFailure(
                        f"mode={mode_name} daemon[{idx}] exited prematurely (rc={rc})\n"
                        f"stdout={out}\nstderr={err}"
                    )
            if all(os.path.exists(m) and os.path.getsize(m) > 0 for m in markers):
                break
            time.sleep(0.05)
        else:
            for p in procs:
                if p.poll() is None:
                    p.kill()
            raise BenchFailure(f"mode={mode_name}: timed out waiting for {k_daemons} daemon readiness markers")

        raw_snapshots: List[Tuple[int, str, str]] = []
        try:
            for p in procs:
                if p.poll() is not None:
                    raise BenchFailure(f"mode={mode_name}: daemon pid={p.pid} exited before smaps read")
                with open(f"/proc/{p.pid}/smaps", "r", encoding="utf-8", errors="replace") as sf:
                    smaps_text = sf.read()
                with open(f"/proc/{p.pid}/maps", "r", encoding="utf-8", errors="replace") as mf:
                    maps_text = mf.read()
                if p.poll() is not None:
                    raise BenchFailure(f"mode={mode_name}: daemon pid={p.pid} exited during smaps read")
                raw_snapshots.append((p.pid, smaps_text, maps_text))
        finally:
            outputs: List[Tuple[int, str, str]] = []
            for p in procs:
                out, err = p.communicate(timeout=daemon_sec + 15)
                outputs.append((p.returncode, out, err))

        for idx, (rc, out, err) in enumerate(outputs):
            if rc != 0:
                raise BenchFailure(
                    f"mode={mode_name} daemon[{idx}] failed with rc={rc}\nstdout={out}\nstderr={err}"
                )
            if "BENCH_DAEMON_OK" not in out or "v=13.0" not in out:
                raise BenchFailure(
                    f"mode={mode_name} daemon[{idx}] missing BENCH_DAEMON_OK output: {out!r}"
                )

    per_pid: List[Dict[str, Any]] = []
    libc_inodes = set()
    compat_inodes = set()
    dbt_inodes = set()

    for pid, smaps_text, maps_text in raw_snapshots:
        vmas = parse_smaps(smaps_text)
        check_no_i386_libs(pid, vmas, maps_text)

        libc_info = inspect_shared_library(pid, vmas, "libc.so.6", require_x32_dir=True)
        libc_inodes.add(libc_info["inode"])

        compat_info = None
        if require_compat:
            compat_info = inspect_shared_library(pid, vmas, "libnewminhh_compat.so")
            compat_inodes.add(compat_info["inode"])

        dbt_info = None
        if require_dbt:
            dbt_info = inspect_shared_library(pid, vmas, "libnewminhh_dbt.so")
            dbt_inodes.add(dbt_info["inode"])

        tot_rss = sum(v["metrics"].get("Rss", 0) for v in vmas)
        tot_pss = sum(v["metrics"].get("Pss", 0) for v in vmas)
        tot_sc = sum(v["metrics"].get("Shared_Clean", 0) for v in vmas)
        tot_sd = sum(v["metrics"].get("Shared_Dirty", 0) for v in vmas)
        tot_pc = sum(v["metrics"].get("Private_Clean", 0) for v in vmas)
        tot_pd = sum(v["metrics"].get("Private_Dirty", 0) for v in vmas)

        per_pid.append({
            "pid": pid,
            "rss_kb": tot_rss,
            "pss_kb": tot_pss,
            "shared_clean_kb": tot_sc,
            "shared_dirty_kb": tot_sd,
            "private_clean_kb": tot_pc,
            "private_dirty_kb": tot_pd,
            "libc": libc_info,
            "compat": compat_info,
            "dbt": dbt_info,
        })

    if len(libc_inodes) != 1:
        raise BenchFailure(f"mode={mode_name}: libc.so.6 inode mismatch across {k_daemons} PIDs: {libc_inodes}")
    if require_compat and len(compat_inodes) != 1:
        raise BenchFailure(
            f"mode={mode_name}: libnewminhh_compat.so inode mismatch across {k_daemons} PIDs: {compat_inodes}"
        )
    if require_dbt and len(dbt_inodes) != 1:
        raise BenchFailure(
            f"mode={mode_name}: libnewminhh_dbt.so inode mismatch across {k_daemons} PIDs: {dbt_inodes}"
        )

    fleet_sum_pss = sum(x["pss_kb"] for x in per_pid)
    fleet_sum_rss = sum(x["rss_kb"] for x in per_pid)
    if fleet_sum_pss >= fleet_sum_rss:
        raise BenchFailure(
            f"mode={mode_name}: expected fleet sum(PSS)={fleet_sum_pss} kB < sum(RSS)={fleet_sum_rss} kB"
        )

    savings_kb = fleet_sum_rss - fleet_sum_pss
    savings_pct = (savings_kb / fleet_sum_rss * 100.0) if fleet_sum_rss > 0 else 0.0

    return {
        "mode": mode_name,
        "k_daemons": k_daemons,
        "mean_rss_kb": round(statistics.mean(x["rss_kb"] for x in per_pid), 1),
        "mean_pss_kb": round(statistics.mean(x["pss_kb"] for x in per_pid), 1),
        "mean_shared_clean_kb": round(statistics.mean(x["shared_clean_kb"] for x in per_pid), 1),
        "mean_private_clean_kb": round(statistics.mean(x["private_clean_kb"] for x in per_pid), 1),
        "mean_private_dirty_kb": round(statistics.mean(x["private_dirty_kb"] for x in per_pid), 1),
        "fleet_sum_pss_kb": fleet_sum_pss,
        "fleet_sum_rss_kb": fleet_sum_rss,
        "fleet_ram_saved_kb": savings_kb,
        "fleet_savings_pct": round(savings_pct, 2),
        "libc_inode": next(iter(libc_inodes)),
        "libc_path": per_pid[0]["libc"]["paths"][0],
        "libc_shared_clean_kb": round(statistics.mean(x["libc"]["shared_clean_kb"] for x in per_pid), 1),
        "libc_private_dirty_kb": 0,
        "compat_inode": next(iter(compat_inodes)) if require_compat else None,
        "compat_shared_clean_kb": (
            round(statistics.mean(x["compat"]["shared_clean_kb"] for x in per_pid), 1)
            if require_compat
            else None
        ),
        "compat_private_dirty_kb": 0 if require_compat else None,
        "dbt_inode": next(iter(dbt_inodes)) if require_dbt else None,
        "dbt_shared_clean_kb": (
            round(statistics.mean(x["dbt"]["shared_clean_kb"] for x in per_pid), 1)
            if require_dbt
            else None
        ),
        "pids": per_pid,
    }


def run_timing_workload(
    workload_name: str,
    workload_args: List[str],
    modes: List[Tuple[str, List[str]]],
    samples: int,
) -> Dict[str, Any]:
    results: Dict[str, Any] = {}
    ref_compute_line: Optional[str] = None

    for mode_name, cmd_prefix in modes:
        timings_ms: List[float] = []
        last_line = ""
        cmd = cmd_prefix + workload_args

        for s_idx in range(samples):
            t0 = time.perf_counter()
            proc = subprocess.run(cmd, capture_output=True, text=True)
            t1 = time.perf_counter()
            elapsed_ms = (t1 - t0) * 1000.0
            if proc.returncode != 0:
                raise BenchFailure(
                    f"timing workload={workload_name} mode={mode_name} sample={s_idx} "
                    f"failed (rc={proc.returncode}): stdout={proc.stdout!r} stderr={proc.stderr!r}"
                )
            out_lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
            if not out_lines:
                raise BenchFailure(f"timing workload={workload_name} mode={mode_name} produced empty stdout")
            line = out_lines[-1]
            if workload_name in ("compute", "cold_start"):
                expected_iters = workload_args[1]
                m = re.match(rf"^BENCH_COMPUTE_OK csum=(0x[0-9a-f]{{8}}) iters={re.escape(expected_iters)}$", line)
                if not m:
                    raise BenchFailure(
                        f"workload={workload_name} mode={mode_name} unexpected output: {line!r}"
                    )
                if ref_compute_line is None:
                    ref_compute_line = line
                elif line != ref_compute_line:
                    raise BenchFailure(
                        f"workload={workload_name} mode={mode_name} checksum mismatch: "
                        f"got {line!r}, expected {ref_compute_line!r}"
                    )
            elif workload_name == "libc_heavy":
                expected_iters = workload_args[1]
                if not re.match(rf"^BENCH_LIBC_OK csum=0x[0-9a-f]{{8}} iters={re.escape(expected_iters)}$", line):
                    raise BenchFailure(
                        f"workload={workload_name} mode={mode_name} unexpected output: {line!r}"
                    )
            last_line = line
            timings_ms.append(elapsed_ms)

        med = statistics.median(timings_ms)
        mean = statistics.mean(timings_ms)
        results[mode_name] = {
            "samples": samples,
            "timings_ms": [round(x, 3) for x in timings_ms],
            "median_ms": round(med, 3),
            "mean_ms": round(mean, 3),
            "min_ms": round(min(timings_ms), 3),
            "max_ms": round(max(timings_ms), 3),
            "sample_output": last_line,
        }

    native_med = results["native_x32"]["median_ms"]
    native_mean = results["native_x32"]["mean_ms"]
    for mode_name in results:
        r = results[mode_name]
        r["median_ratio_vs_x32"] = round(r["median_ms"] / native_med, 3) if native_med > 0 else 0.0
        r["mean_ratio_vs_x32"] = round(r["mean_ms"] / native_mean, 3) if native_mean > 0 else 0.0

    return {
        "workload": workload_name,
        "args": workload_args,
        "deterministic_ref": ref_compute_line,
        "modes": results,
    }


def print_human_report(report: Dict[str, Any]) -> None:
    print("==========================================================================")
    print("newminhh Part 5 Verification: /proc/<pid>/smaps Sharing, RSS & Timing")
    print("==========================================================================")
    print(f"Environment: arch={report['env']['arch']} kernel={report['env']['kernel']}")
    print(f"Shared libc.so.6 across all modes: {report['sharing_summary']['libc_path']} "
          f"(inode={report['sharing_summary']['libc_inode']})")
    if report["sharing_summary"]["compat_inode"] is not None:
        print(f"Shared libnewminhh_compat.so across patched/dbt modes: "
              f"inode={report['sharing_summary']['compat_inode']}")
    print("")

    print("--- Concurrent Daemon Memory & Page Sharing (K="
          f"{report['memory'][0]['k_daemons']} processes) ---")
    hdr = (
        f"{'Mode':<16} {'RSS(kB)':>8} {'PSS(kB)':>8} {'ShClean':>8} "
        f"{'PrivDty':>8} {'SumPSS/SumRSS':>15} {'Saved%':>7} "
        f"{'libcSh(kB)':>11} {'compatSh':>9}"
    )
    print(hdr)
    print("-" * len(hdr))
    for m in report["memory"]:
        sum_pair = f"{m['fleet_sum_pss_kb']}/{m['fleet_sum_rss_kb']}"
        compat_s = f"{m['compat_shared_clean_kb']:.1f}" if m["compat_shared_clean_kb"] is not None else "n/a"
        print(
            f"{m['mode']:<16} "
            f"{m['mean_rss_kb']:>8.1f} "
            f"{m['mean_pss_kb']:>8.1f} "
            f"{m['mean_shared_clean_kb']:>8.1f} "
            f"{m['mean_private_dirty_kb']:>8.1f} "
            f"{sum_pair:>15} "
            f"{m['fleet_savings_pct']:>6.1f}% "
            f"{m['libc_shared_clean_kb']:>11.1f} "
            f"{compat_s:>9}"
        )
    print("")

    print(f"--- Execution Timing Benchmarks (samples={report['samples']}) ---")
    thdr = (
        f"{'Workload':<12} {'Mode':<16} {'Median(ms)':>10} {'Mean(ms)':>10} "
        f"{'Min..Max(ms)':>18} {'Ratio':>7} {'Output'}"
    )
    print(thdr)
    print("-" * len(thdr))
    for w in report["timings"]:
        wname = w["workload"]
        for mode_name, r in w["modes"].items():
            mm = f"{r['min_ms']:.2f}..{r['max_ms']:.2f}"
            print(
                f"{wname:<12} "
                f"{mode_name:<16} "
                f"{r['median_ms']:>10.2f} "
                f"{r['mean_ms']:>10.2f} "
                f"{mm:>18} "
                f"{r['median_ratio_vs_x32']:>6.2f}x "
                f"{r['sample_output']}"
            )
    print("")
    print("ALL_PART5_BENCH_CHECKS_PASSED")


def main() -> int:
    repo = default_repo_root()
    parser = argparse.ArgumentParser(
        description="Verify /proc/<pid>/smaps sharing, RSS/PSS, and timing inside Debian x32 VM"
    )
    parser.add_argument(
        "--native",
        default=os.path.join(repo, "tests", "corpus", "build", "x32", "bench_workload"),
        help="Path to native x32 bench_workload",
    )
    parser.add_argument(
        "--patched",
        required=True,
        help="Path to statically patched x32 bench_workload (from newminhh-patch)",
    )
    parser.add_argument(
        "--hybrid",
        default=None,
        help="Optional path to --hybrid-fallback patched bench_workload",
    )
    parser.add_argument(
        "--i386",
        default=os.path.join(repo, "tests", "corpus", "build", "i386", "bench_workload"),
        help="Path to unpatched i386 bench_workload",
    )
    parser.add_argument(
        "--runner",
        default="/usr/local/bin/newminhh-run",
        help="Path to newminhh-run",
    )
    parser.add_argument("--daemons", type=int, default=4, help="Concurrent daemons K (default: 4)")
    parser.add_argument("--daemon-sec", type=int, default=4, help="Daemon sleep seconds (default: 4)")
    parser.add_argument("--samples", type=int, default=5, help="Timing samples per workload (default: 5)")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON output")
    args = parser.parse_args()

    if args.daemons < 2:
        sys.stderr.write("[newminhh:bench] ERROR: --daemons must be >= 2\n")
        return 2
    if args.samples < 1:
        sys.stderr.write("[newminhh:bench] ERROR: --samples must be >= 1\n")
        return 2

    for label, p in (
        ("native", args.native),
        ("patched", args.patched),
        ("i386", args.i386),
        ("runner", args.runner),
    ):
        if not os.path.isfile(p) or not os.access(p, os.X_OK):
            sys.stderr.write(f"[newminhh:bench] ERROR: {label} binary missing or not executable: {p}\n")
            return 1
    if args.hybrid and (not os.path.isfile(args.hybrid) or not os.access(args.hybrid, os.X_OK)):
        sys.stderr.write(f"[newminhh:bench] ERROR: hybrid binary missing or not executable: {args.hybrid}\n")
        return 1

    modes_spec: List[Tuple[str, List[str], bool, bool]] = [
        ("native_x32", [args.native], False, False),
        ("patched_compat", [args.patched], True, False),
    ]
    if args.hybrid:
        modes_spec.append(("patched_hybrid", [args.hybrid], True, False))
    modes_spec.append(("dbt_runtime", [args.runner, args.i386], True, True))

    try:
        if hasattr(os, "sync"):
            os.sync()
        memory_results: List[Dict[str, Any]] = []
        for mode_name, cmd_prefix, req_compat, req_dbt in modes_spec:
            res = measure_concurrent_daemons(
                mode_name=mode_name,
                cmd_prefix=cmd_prefix,
                k_daemons=args.daemons,
                daemon_sec=args.daemon_sec,
                require_compat=req_compat,
                require_dbt=req_dbt,
            )
            memory_results.append(res)

        all_libc_inodes = {m["libc_inode"] for m in memory_results}
        if len(all_libc_inodes) != 1:
            raise BenchFailure(
                f"libc.so.6 inode mismatch across modes: "
                f"{[(m['mode'], m['libc_inode']) for m in memory_results]}"
            )

        compat_inodes = {m["compat_inode"] for m in memory_results if m["compat_inode"] is not None}
        if len(compat_inodes) != 1:
            raise BenchFailure(
                f"libnewminhh_compat.so inode mismatch across modes: "
                f"{[(m['mode'], m['compat_inode']) for m in memory_results]}"
            )

        timing_modes = [(m[0], m[1]) for m in modes_spec]
        workloads = [
            ("compute", ["--compute", "5000000"]),
            ("libc_heavy", ["--libc-heavy", "20000"]),
            ("cold_start", ["--compute", "1000"]),
        ]
        timing_results = [
            run_timing_workload(wname, wargs, timing_modes, args.samples)
            for wname, wargs in workloads
        ]

        uname_res = os.uname()
        dpkg_arch = subprocess.run(
            ["dpkg", "--print-architecture"], capture_output=True, text=True
        ).stdout.strip()

        report = {
            "env": {
                "arch": dpkg_arch or uname_res.machine,
                "kernel": uname_res.release,
                "node": uname_res.nodename,
            },
            "samples": args.samples,
            "sharing_summary": {
                "libc_inode": next(iter(all_libc_inodes)),
                "libc_path": memory_results[0]["libc_path"],
                "compat_inode": next(iter(compat_inodes)) if compat_inodes else None,
            },
            "memory": memory_results,
            "timings": timing_results,
        }

        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print_human_report(report)
        return 0

    except BenchFailure as exc:
        sys.stderr.write(f"[newminhh:bench] ERROR: {exc}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
