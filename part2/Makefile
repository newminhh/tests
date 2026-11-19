CC := gcc
CFLAGS := -O2 -Wall -Wextra -Werror=implicit-function-declaration -I../../src/common
ASFLAGS :=
COMPAT_DIR := ../../src/compat
LDFLAGS := -L$(COMPAT_DIR) -Wl,-rpath,$(COMPAT_DIR) -lnewminhh_compat -lm -lpthread -ldl

TARGET := test_compat

.PHONY: all clean run

all: $(TARGET)

$(TARGET): test_compat.c test_compat_asm.S $(COMPAT_DIR)/libnewminhh_compat.so
	$(CC) $(CFLAGS) test_compat.c test_compat_asm.S $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
