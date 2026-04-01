# ============================================================
#  O Language Compiler - Makefile  (Linux/macOS)
#  Z-TEAM | C23
#
#  Usage:
#    make          - build oc
#    make debug    - sanitizer build
#    make clean    - clean up
#    make test-jit - run JIT tests
# ============================================================

CC      := gcc
STD     := -std=gnu2x
INCDIRS := -Iinclude

RFLAGS  := -O2 -march=native -fomit-frame-pointer \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -DNDEBUG
DFLAGS  := -O0 -g3 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS  ?= $(RFLAGS)
LDFLAGS := -ldl
TARGET  := oc

SRCS := $(wildcard src/frontend/*.c) \
        $(wildcard src/ir/*.c)       \
        $(wildcard src/backend/*.c)  \
        $(wildcard src/jit/*.c)      \
        $(wildcard src/output/obj/*.c)     \
        $(wildcard src/output/elf/*.c)     \
        $(wildcard src/output/windows/*.c) \
        $(wildcard src/output/iso/*.c)     \
        src/driver.c

OBJS := $(patsubst src/%.c, build/%.o, $(SRCS))

.PHONY: all debug clean test test-jit test-linux test-windows test-iso

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(STD) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@printf '\033[38;2;88;240;27m[Z-TEAM]\033[0m Built %s\n' "$@"

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(CFLAGS) $(INCDIRS) -c -o $@ $<

debug:
	$(MAKE) CFLAGS="$(DFLAGS)" all

clean:
	rm -rf build $(TARGET)

test: all test-jit

test-jit: all
	@echo "--- hello ---" && ./oc examples/hello.o --jit
	@echo "--- fib ---"   && ./oc examples/fib.o --jit | head -12
	@echo "--- fizzbuzz ---" && ./oc examples/fizzbuzz.o --jit

test-linux: all
	@./oc examples/hello.o -o /tmp/o_test_elf && /tmp/o_test_elf
	@./oc examples/hello.o -o /tmp/o_test.so --fmt so

test-windows: all
	@./oc examples/hello.o -o /tmp/o_test.exe --target x86_64-windows --fmt exe -v
	@./oc examples/hello.o -o /tmp/o_test.dll --target x86_64-windows --fmt dll -v

test-iso: all
	@./oc --grub-check && ./oc examples/hello.o -o /tmp/o_test.iso --fmt iso || true
