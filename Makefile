# Builds the tools into bin/. v2replay builds anywhere with a C99 compiler.
# The Windows test programs need a MinGW cross compiler and NASM: see tools/a3dtest
# and tools/sbtest.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CPPFLAGS += -Icore
CORE    = core/snd_vortex2_core.c

all: bin/v2replay

bin:
	mkdir -p bin

bin/v2replay: tools/v2replay.c $(CORE) core/*.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tools/v2replay.c $(CORE) -lm

a3dtest:
	cd tools/a3dtest && ./build.sh

clean:
	rm -rf bin

.PHONY: all a3dtest clean
