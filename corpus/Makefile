ifeq ($(origin CC),default)
  CC := clang
endif
CSTD ?= gnu23
IS_CLANG := $(shell $(CC) -dM -E - < /dev/null 2>/dev/null | grep -c '__clang__')

NASM := nasm
STRIP := strip
PYTHON3 := python3

BUILD_ROOT := build
I386_DIR := $(BUILD_ROOT)/i386
I386_NEG_DIR := $(I386_DIR)/negatives
X32_DIR := $(BUILD_ROOT)/x32

COMMON_DIR ?= ../../src/common

CFLAGS_COMMON := -O2 -std=$(CSTD) -Wall -Wextra -Wpedantic -Wstrict-prototypes -Werror=implicit-function-declaration -I$(COMMON_DIR)
ifeq ($(IS_CLANG),1)
  CFLAGS_COMMON += -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
endif
CFLAGS_I386 := -m32 $(CFLAGS_COMMON)
CFLAGS_X32 := $(CFLAGS_COMMON)

I386_CLI_BINS := \
	$(I386_DIR)/cli_libc_pie \
	$(I386_DIR)/cli_libc_nopie \
	$(I386_DIR)/cli_switch_jumptable_pie \
	$(I386_DIR)/cli_switch_jumptable_nopie \
	$(I386_DIR)/cli_pic_thunks \
	$(I386_DIR)/cli_structs_abi \
	$(I386_DIR)/cli_structs_abi64 \
	$(I386_DIR)/cli_callbacks \
	$(I386_DIR)/cli_syscall_int80 \
	$(I386_DIR)/cli_syscall_sysenter \
	$(I386_DIR)/cli_init_fini \
	$(I386_DIR)/cli_dlopen_client \
	$(I386_DIR)/cli_stripped

I386_SOS := \
	$(I386_DIR)/libbasic_pic.so \
	$(I386_DIR)/liblifecycle_abi.so \
	$(I386_DIR)/libplugin_stripped.so

I386_EXTRAS := \
	$(I386_DIR)/bench_workload \
	$(I386_DIR)/neg_dlsym_missing

I386_NEGS := \
	$(I386_NEG_DIR)/bad_trunc_ehdr.elf \
	$(I386_NEG_DIR)/bad_magic.bin \
	$(I386_NEG_DIR)/bad_arch_arm.elf \
	$(I386_NEG_DIR)/bad_class64_i386.elf \
	$(I386_NEG_DIR)/bad_phdr_oob.elf \
	$(I386_NEG_DIR)/bad_corrupt_dyn.elf \
	$(I386_NEG_DIR)/bad_priv_insn.elf \
	$(I386_NEG_DIR)/bad_bcd_insn.elf

X32_CLI_BINS := \
	$(X32_DIR)/cli_libc_pie \
	$(X32_DIR)/cli_libc_nopie \
	$(X32_DIR)/cli_switch_jumptable_pie \
	$(X32_DIR)/cli_switch_jumptable_nopie \
	$(X32_DIR)/cli_pic_thunks \
	$(X32_DIR)/cli_structs_abi \
	$(X32_DIR)/cli_structs_abi64 \
	$(X32_DIR)/cli_callbacks \
	$(X32_DIR)/cli_syscall_int80 \
	$(X32_DIR)/cli_syscall_sysenter \
	$(X32_DIR)/cli_init_fini \
	$(X32_DIR)/cli_dlopen_client \
	$(X32_DIR)/cli_stripped

X32_SOS := \
	$(X32_DIR)/libbasic_pic.so \
	$(X32_DIR)/liblifecycle_abi.so \
	$(X32_DIR)/libplugin_stripped.so

X32_EXTRAS := \
	$(X32_DIR)/x32_so_host_driver \
	$(X32_DIR)/bench_workload \
	$(X32_DIR)/neg_dlsym_missing

.PHONY: all i386 x32 verify-i386 clean

all: i386

$(I386_DIR) $(I386_NEG_DIR) $(X32_DIR):
	mkdir -p $@

# --- i386 CLI Executables ---
$(I386_DIR)/cli_libc_pie: src/cli_libc.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIE -pie $< -o $@

$(I386_DIR)/cli_libc_nopie: src/cli_libc.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fno-pie -no-pie $< -o $@

$(I386_DIR)/cli_switch_jumptable_pie: src/cli_switch_jumptable.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIE -pie $< -o $@

$(I386_DIR)/cli_switch_jumptable_nopie: src/cli_switch_jumptable.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fno-pie -no-pie $< -o $@

$(I386_DIR)/cli_pic_thunks: src/cli_pic_thunks.c src/cli_pic_thunks_helper.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIE -pie $^ -o $@

$(I386_DIR)/cli_structs_abi: src/cli_structs_abi.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -o $@

$(I386_DIR)/cli_structs_abi64: src/cli_structs_abi.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -D_FILE_OFFSET_BITS=64 $< -o $@

$(I386_DIR)/cli_callbacks: src/cli_callbacks.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -pthread $< -o $@

$(I386_DIR)/cli_syscall_int80: src/cli_syscall_int80.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -o $@

$(I386_DIR)/cli_syscall_sysenter_i386.o: src/cli_syscall_sysenter_i386.asm | $(I386_DIR)
	$(NASM) -f elf32 $< -o $@

$(I386_DIR)/cli_syscall_sysenter: src/cli_syscall_sysenter.c $(I386_DIR)/cli_syscall_sysenter_i386.o | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $^ -o $@

$(I386_DIR)/cli_init_fini: src/cli_init_fini.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -o $@

$(I386_DIR)/cli_dlopen_client: src/cli_dlopen_client.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -ldl -lm -o $@

$(I386_DIR)/cli_stripped: src/cli_stripped.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -pthread $< -o $@
	$(STRIP) --strip-all $@

# --- i386 Shared Objects ---
$(I386_DIR)/libbasic_pic.so: src/libbasic_pic.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIC -shared $< -o $@

$(I386_DIR)/liblifecycle_abi.so: src/liblifecycle_abi.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIC -shared $< -o $@

$(I386_DIR)/libplugin_stripped.so: src/libplugin_stripped.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) -fPIC -shared $< -o $@
	$(STRIP) --strip-unneeded $@

# --- i386 Benchmark & Negative Helpers ---
$(I386_DIR)/bench_workload: src/bench_workload.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -lm -o $@

$(I386_DIR)/neg_dlsym_missing: src/neg_dlsym_missing.c | $(I386_DIR)
	$(CC) $(CFLAGS_I386) $< -ldl -o $@

$(I386_NEGS): $(I386_DIR)/cli_libc_pie make_negatives.py | $(I386_NEG_DIR)
	$(PYTHON3) make_negatives.py --template $(I386_DIR)/cli_libc_pie --outdir $(I386_NEG_DIR)

i386: $(I386_CLI_BINS) $(I386_SOS) $(I386_EXTRAS) $(I386_NEGS)

# --- Native x32 Baselines (Invoke inside Debian x32 VM) ---
$(X32_DIR)/cli_libc_pie: src/cli_libc.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIE -pie $< -o $@

$(X32_DIR)/cli_libc_nopie: src/cli_libc.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fno-pie -no-pie $< -o $@

$(X32_DIR)/cli_switch_jumptable_pie: src/cli_switch_jumptable.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIE -pie $< -o $@

$(X32_DIR)/cli_switch_jumptable_nopie: src/cli_switch_jumptable.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fno-pie -no-pie $< -o $@

$(X32_DIR)/cli_pic_thunks: src/cli_pic_thunks.c src/cli_pic_thunks_helper.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIE -pie $^ -o $@

$(X32_DIR)/cli_structs_abi: src/cli_structs_abi.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -o $@

$(X32_DIR)/cli_structs_abi64: src/cli_structs_abi.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -D_FILE_OFFSET_BITS=64 $< -o $@

$(X32_DIR)/cli_callbacks: src/cli_callbacks.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -pthread $< -o $@

$(X32_DIR)/cli_syscall_int80: src/cli_syscall_int80.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -o $@

$(X32_DIR)/cli_syscall_sysenter: src/cli_syscall_sysenter.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -o $@

$(X32_DIR)/cli_init_fini: src/cli_init_fini.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -o $@

$(X32_DIR)/cli_dlopen_client: src/cli_dlopen_client.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -ldl -lm -o $@

$(X32_DIR)/cli_stripped: src/cli_stripped.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -pthread $< -o $@
	$(STRIP) --strip-all $@

$(X32_DIR)/libbasic_pic.so: src/libbasic_pic.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIC -shared $< -o $@

$(X32_DIR)/liblifecycle_abi.so: src/liblifecycle_abi.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIC -shared $< -o $@

$(X32_DIR)/libplugin_stripped.so: src/libplugin_stripped.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) -fPIC -shared $< -o $@
	$(STRIP) --strip-unneeded $@

$(X32_DIR)/x32_so_host_driver: src/x32_so_host_driver.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -ldl -o $@

$(X32_DIR)/bench_workload: src/bench_workload.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -lm -o $@

$(X32_DIR)/neg_dlsym_missing: src/neg_dlsym_missing.c | $(X32_DIR)
	$(CC) $(CFLAGS_X32) $< -ldl -o $@

x32: $(X32_CLI_BINS) $(X32_SOS) $(X32_EXTRAS)

verify-i386: i386
	@for f in $(I386_CLI_BINS) $(I386_SOS) $(I386_EXTRAS); do \
		file "$$f"; \
		readelf -h "$$f" | grep -E "Class:|Machine:" | xargs echo "  ->"; \
	done
	@echo "--- Negative Fixtures ---"
	@for f in $(I386_NEGS); do \
		ls -lh "$$f"; \
		file "$$f"; \
	done

clean:
	rm -rf $(BUILD_ROOT)
