#!/usr/bin/env python3
"""Generate N01-N08 malformed and unsupported negative test fixtures for newminhh."""

import argparse
import os
import shutil
import struct
import subprocess
import tempfile


def write_file(path: str, data: bytes) -> None:
    with open(path, "wb") as f:
        f.write(data)


def build_asm_elf32(asm_src: str, out_path: str) -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        asm_path = os.path.join(tmpdir, "prog.asm")
        obj_path = os.path.join(tmpdir, "prog.o")
        with open(asm_path, "w", encoding="utf-8") as f:
            f.write(asm_src)
        subprocess.check_call(["nasm", "-f", "elf32", asm_path, "-o", obj_path])
        subprocess.check_call(
            ["gcc", "-m32", "-nostdlib", "-no-pie", obj_path, "-o", out_path]
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate N01-N08 negative fixtures")
    parser.add_argument(
        "--template",
        required=True,
        help="Path to a valid dynamically linked i386 ELF32 executable",
    )
    parser.add_argument("--outdir", required=True, help="Output directory")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    with open(args.template, "rb") as f:
        tpl = f.read()

    if len(tpl) < 52 or tpl[:4] != b"\x7fELF" or tpl[4] != 1:
        raise SystemExit(f"Template {args.template} is not a valid ELF32 binary")

    # N01: bad_trunc_ehdr.elf (32-byte truncated ELF header)
    write_file(os.path.join(args.outdir, "bad_trunc_ehdr.elf"), tpl[:32])

    # N02: bad_magic.bin (256-byte non-ELF file)
    header = b"NOT_AN_ELF_BINARY_NEWMINHH_NEGATIVE_FIXTURE_"
    pad = bytes((i & 0xFF) for i in range(256 - len(header)))
    write_file(os.path.join(args.outdir, "bad_magic.bin"), header + pad)

    # N03: bad_arch_arm.elf (ELF32 with e_machine = EM_ARM = 40)
    n03 = bytearray(tpl)
    struct.pack_into("<H", n03, 18, 40)
    write_file(os.path.join(args.outdir, "bad_arch_arm.elf"), bytes(n03))

    # N04: bad_class64_i386.elf (EI_CLASS = ELFCLASS64 = 2 with e_machine = EM_386 = 3)
    n04 = bytearray(tpl)
    n04[4] = 2  # ELFCLASS64
    struct.pack_into("<H", n04, 18, 3)  # EM_386
    write_file(os.path.join(args.outdir, "bad_class64_i386.elf"), bytes(n04))

    # N05: bad_phdr_oob.elf (PT_LOAD p_offset and p_filesz beyond EOF)
    n05 = bytearray(tpl)
    e_phoff = struct.unpack_from("<I", n05, 28)[0]
    e_phentsize = struct.unpack_from("<H", n05, 42)[0]
    e_phnum = struct.unpack_from("<H", n05, 44)[0]
    patched_phdr = False
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", n05, off)[0]
        if p_type == 1:  # PT_LOAD
            struct.pack_into("<I", n05, off + 4, len(n05) + 0x10000)   # p_offset
            struct.pack_into("<I", n05, off + 16, 0x20000)             # p_filesz
            patched_phdr = True
            break
    if not patched_phdr:
        raise SystemExit("No PT_LOAD found in template for N05")
    write_file(os.path.join(args.outdir, "bad_phdr_oob.elf"), bytes(n05))

    # N06: bad_corrupt_dyn.elf (PT_DYNAMIC DT_STRTAB / DT_SYMTAB corrupted)
    n06 = bytearray(tpl)
    patched_dyn = False
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", n06, off)[0]
        if p_type == 2:  # PT_DYNAMIC
            dyn_off = struct.unpack_from("<I", n06, off + 4)[0]
            dyn_sz = struct.unpack_from("<I", n06, off + 16)[0]
            for pos in range(dyn_off, dyn_off + dyn_sz, 8):
                d_tag = struct.unpack_from("<i", n06, pos)[0]
                if d_tag == 0:
                    break
                if d_tag == 5:  # DT_STRTAB
                    struct.pack_into("<I", n06, pos + 4, 0xDEADBEEF)
                    patched_dyn = True
                elif d_tag == 6:  # DT_SYMTAB
                    struct.pack_into("<I", n06, pos + 4, 0xDEADC0DE)
                    patched_dyn = True
            break
    if not patched_dyn:
        raise SystemExit("No PT_DYNAMIC/DT_STRTAB found in template for N06")
    write_file(os.path.join(args.outdir, "bad_corrupt_dyn.elf"), bytes(n06))

    # N07: bad_priv_insn.elf (hlt, ud2, in al, dx)
    if not shutil.which("nasm") or not shutil.which("gcc"):
        raise SystemExit("nasm and gcc are required to assemble N07/N08")
    n07_asm = """\
section .text
global _start
_start:
    hlt
    ud2
    in al, dx
    mov eax, 252
    xor ebx, ebx
    int 0x80
section .note.GNU-stack noalloc noexec nowrite progbits
"""
    build_asm_elf32(n07_asm, os.path.join(args.outdir, "bad_priv_insn.elf"))

    # N08: bad_bcd_insn.elf (Long-Mode invalid BCD opcodes: aaa, daa)
    n08_asm = """\
section .text
global _start
_start:
    mov eax, 0x00000019
    aaa
    daa
    mov eax, 252
    xor ebx, ebx
    int 0x80
section .note.GNU-stack noalloc noexec nowrite progbits
"""
    build_asm_elf32(n08_asm, os.path.join(args.outdir, "bad_bcd_insn.elf"))

    print(f"Generated N01-N08 negative fixtures in {args.outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
