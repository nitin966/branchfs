# Makefile for libbranch — userspace prototype of the branch() syscall.
#
# Produces:
#   libbranch.so   — shared library
#   libbranch.a    — static library
#   parallel       — example program (requires BranchFS mounted to run)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic
CFLAGS  += -D_GNU_SOURCE -Iinclude

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

.PHONY: all clean examples

examples: examples/parallel

clean:
	rm -f $(OBJ) libbranch.so libbranch.a examples/parallel
