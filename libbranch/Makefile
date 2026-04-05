# Makefile for libbranch — userspace prototype of the branch() syscall.
#
# Produces:
#   libbranch.so        — shared library
#   libbranch.a         — static library
#   examples/parallel   — example program (requires BranchFS mounted to run)
#   tests/test_branch   — test binary (requires BranchFS mounted to run)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
CFLAGS  += -D_GNU_SOURCE -Iinclude
# Suppress false-positive truncation warnings: paths come from /tmp and are
# well within PATH_MAX; GCC can't prove this statically.
CFLAGS  += -Wno-format-truncation -Wno-stringop-truncation

SRC     = src/branch.c
OBJ     = src/branch.o

all: libbranch.so libbranch.a

libbranch.so: $(SRC)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $^

libbranch.a: $(OBJ)
	ar rcs $@ $^

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $^

examples/parallel: examples/parallel.c libbranch.a
	$(CC) $(CFLAGS) -o $@ $< libbranch.a

tests/test_branch: tests/test_branch.c libbranch.a
	$(CC) $(CFLAGS) -Wno-format-truncation -Wno-stringop-truncation -o $@ $< libbranch.a

.PHONY: all clean examples tests

examples: examples/parallel

tests: tests/test_branch

clean:
	rm -f $(OBJ) libbranch.so libbranch.a examples/parallel tests/test_branch
