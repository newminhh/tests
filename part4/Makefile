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

ifeq ($(IS_CLANG),1)
  CFLAGS += -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
endif

LDFLAGS := -no-pie -fuse-ld=bfd -Wl,-Ttext-segment=0x70800000 -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -L$(DBT_DIR) -L$(COMPAT_DIR) -L$(LIBDIR) -Wl,-rpath,$(LIBDIR):$(DBT_DIR):$(COMPAT_DIR)
LIBS := -lnewminhh_dbt -lnewminhh_compat -ldl -lpthread

COMMON_HDRS := $(COMMON_DIR)/newminhh_cstd.h $(COMMON_DIR)/newminhh_abi.h $(COMMON_DIR)/newminhh_log.h

.PHONY: all clean analyze

all: test_part4_api test_part4_mt_stress

test_part4_api: test_part4_api.c $(DBT_DIR)/dbt_internal.h $(COMMON_HDRS)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

test_part4_mt_stress: test_part4_mt_stress.c $(DBT_DIR)/dbt_internal.h $(COMMON_HDRS)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@

analyze:
ifeq ($(IS_CLANG),1)
	$(CC) $(CFLAGS) --analyze test_part4_api.c test_part4_mt_stress.c
else
	$(CC) $(CFLAGS) -fanalyzer -c test_part4_api.c -o /dev/null
	$(CC) $(CFLAGS) -fanalyzer -c test_part4_mt_stress.c -o /dev/null
endif

clean:
	rm -f test_part4_api test_part4_mt_stress *.plist
