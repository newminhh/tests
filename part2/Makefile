ifeq ($(origin CC),default)
  CC := clang
endif
CSTD ?= gnu23
IS_CLANG := $(shell $(CC) -dM -E - < /dev/null 2>/dev/null | grep -c '__clang__')

COMMON_DIR ?= ../../src/common
COMPAT_DIR ?= ../../src/compat

CFLAGS ?= -O2 -g
CFLAGS += -std=$(CSTD) -Wall -Wextra -Wpedantic \
          -Wstrict-prototypes -Wold-style-definition -Wshadow -Wundef \
          -Wformat=2 -fstrict-flex-arrays=3 -Werror -no-pie \
          -I$(COMMON_DIR)

ifeq ($(IS_CLANG),1)
  CFLAGS += -Wnullability-completeness -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-octal-literals
endif

LDFLAGS := -L$(COMPAT_DIR) -Wl,-rpath,$(COMPAT_DIR) -Wl,-rpath,'$$ORIGIN/$(COMPAT_DIR)' -lnewminhh_compat -lm -lpthread -ldl

TARGET := test_compat

.PHONY: all clean run analyze

all: $(TARGET)

$(TARGET): test_compat.c test_compat_asm.S $(COMPAT_DIR)/libnewminhh_compat.so $(COMMON_DIR)/newminhh_cstd.h $(COMMON_DIR)/newminhh_abi.h
	$(CC) $(CFLAGS) test_compat.c test_compat_asm.S $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

analyze:
ifeq ($(IS_CLANG),1)
	scan-build -disable-checker nullability.NullPassedToNonnull --status-bugs $(MAKE) clean all
else
	$(MAKE) clean all CFLAGS="$(CFLAGS) -fanalyzer"
endif
