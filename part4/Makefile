CC := gcc
CFLAGS := -O2 -Wall -Wextra -Werror=implicit-function-declaration -I../../src/common -I../../src/dbt
LDFLAGS := -no-pie -Wl,-Ttext-segment=0x70800000 -L../../src/dbt -L/usr/local/lib/x86_64-linux-gnux32 -Wl,-rpath,/usr/local/lib/x86_64-linux-gnux32
LIBS := -lnewminhh_dbt -lnewminhh_compat -ldl -lpthread

.PHONY: all clean

all: test_part4_api test_part4_mt_stress

test_part4_api: test_part4_api.c ../../src/dbt/dbt_internal.h ../../src/common/newminhh_abi.h
	$(CC) $(CFLAGS) test_part4_api.c $(LDFLAGS) $(LIBS) -o $@

test_part4_mt_stress: test_part4_mt_stress.c ../../src/dbt/dbt_internal.h ../../src/common/newminhh_abi.h
	$(CC) $(CFLAGS) test_part4_mt_stress.c $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -f test_part4_api test_part4_mt_stress
