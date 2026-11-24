ifeq ($(origin CC),default)
  CC := clang
endif
CSTD ?= gnu23
IS_CLANG := $(shell $(CC) -dM -E - < /dev/null 2>/dev/null | grep -c '__clang__')

COMMON_DIR ?= ../../src/common
DBT_DIR ?= ../../src/dbt
COMPAT_DIR ?= ../../src/compat
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib/x86_64-linux-gnux32

CFLAGS ?= -O2 -g
CFLAGS += -std=$(CSTD) -Wall -Wextra -Wpedantic \
          -Wstrict-prototypes -Wold-style-definition -Wshadow -Wundef \
          -Wformat=2 -fstrict-flex-arrays=3 -Werror \
          -I$(COMMON_DIR) -I$(DBT_DIR)

CFLAGS_I386 ?= -m32 -O2 -g -std=$(CSTD) -Wall -Wextra -Wpedantic \
               -Wstrict-prototypes -Wold-style-definition -Wshadow -Wundef \
               -Wformat=2 -fstrict-flex-arrays=3 -Werror

ifeq ($(IS_CLANG),1)
  CFLAGS += -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
  CFLAGS_I386 += -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
endif

LDFLAGS := -no-pie -fuse-ld=bfd -Wl,-Ttext-segment=0x70800000 -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -L$(DBT_DIR) -L$(COMPAT_DIR) -L$(LIBDIR) -Wl,-rpath,$(LIBDIR):$(DBT_DIR):$(COMPAT_DIR)
LIBS := -lnewminhh_dbt -lnewminhh_compat -ldl -lpthread

COMMON_HDRS := $(COMMON_DIR)/newminhh_cstd.h $(COMMON_DIR)/newminhh_abi.h $(COMMON_DIR)/newminhh_log.h

.PHONY: all i386 clean distclean analyze

all: test_part4_api test_part4_mt_stress

i386: test_part4_native_libz guest_native_libz

test_part4_api: test_part4_api.c $(DBT_DIR)/dbt_internal.h $(COMMON_HDRS)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

test_part4_mt_stress: test_part4_mt_stress.c $(DBT_DIR)/dbt_internal.h $(COMMON_HDRS)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

test_part4_native_libz: test_part4_native_libz.c
	@if printf 'int main(void){return 0;}\n' | $(CC) -m32 -lz -x c - -o /dev/null 2>/dev/null; then \
		$(CC) $(CFLAGS_I386) $< -lz -o $@; \
	else \
		mkdir -p .libz_stub && \
		printf 'int compress2(void){return 0;} int uncompress(void){return 0;} unsigned long crc32(void){return 0;} const char *zlibVersion(void){return 0;}\n' | \
			$(CC) -m32 -shared -Wl,-soname,libz.so.1 -x c - -o .libz_stub/libz.so && \
		$(CC) $(CFLAGS_I386) -L.libz_stub $< -lz -o $@ && \
		rm -rf .libz_stub; \
	fi

guest_native_libz: test_part4_native_libz
	cp -f $< $@

analyze:
ifeq ($(IS_CLANG),1)
	$(CC) $(CFLAGS) --analyze test_part4_api.c test_part4_mt_stress.c
	$(CC) $(CFLAGS_I386) --analyze test_part4_native_libz.c
else
	$(CC) $(CFLAGS) -fanalyzer -c test_part4_api.c -o /dev/null
	$(CC) $(CFLAGS) -fanalyzer -c test_part4_mt_stress.c -o /dev/null
	$(CC) $(CFLAGS_I386) -fanalyzer -c test_part4_native_libz.c -o /dev/null
endif

clean:
	rm -rf test_part4_api test_part4_mt_stress *.so *.plist .libz_stub

distclean: clean
	rm -f test_part4_native_libz guest_native_libz
