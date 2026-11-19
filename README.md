# `newminhh/tests` — Verification Corpus, Regression Suites & `/proc/<pid>/smaps` Benchmarks

End-to-end test and benchmark suites for the [`newminhh`](https://github.com/newminhh/newminhh) container-less `i386`-on-`x32` compatibility system:

- **`corpus/`** — Part 1 representative `i386` CLI fixtures (`C01`–`C11`), `i386` shared objects (`S01`–`S03`), native `x32` baseline binaries (`x32_so_host_driver`, `bench_workload`), and malformed/negative ELF fixtures (`N01`–`N09`).
- **`part2/`** — Unit and harness tests for [`libnewminhh_compat.so`](https://github.com/newminhh/compat) (`cdecl` ↔ `x32` SysV marshaling, reverse callback trampolines, `int 0x80`/`sysenter` translation, 64-bit struct repacking, and `dlopen`/`dlsym`).
- **`part3/`** — Verification suite for [`newminhh-patch`](https://github.com/newminhh/patcher) static and `--hybrid-fallback` (`REQUIRES_DBT_FALLBACK`) ELF32-to-`x32` binary rewriting.
- **`part4/`** — Verification and multi-threaded stress suite for [`libnewminhh_dbt.so` & `newminhh-run`](https://github.com/newminhh/dbt).
- **`part5/`** — Full clean-build regression runner and live `/proc/<pid>/smaps` + RSS/PSS + timing benchmark harness (`bench_smaps.py`).

See [`newminhh/docs`](https://github.com/newminhh/docs) (`TESTING_AND_VM.md`) for full documentation.
