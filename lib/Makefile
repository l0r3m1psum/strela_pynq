ARCH          ?= arm
CROSS_COMPILE ?=

# NOTE: callers must pass CC for arm, for the reason given in tests/Makefile.
CC ?= $(CROSS_COMPILE)gcc
AR  = $(CROSS_COMPILE)ar

src := $(CURDIR)/..

# OUT, not O, to match tests/Makefile.
OUT ?= $(src)/qemu-run-$(ARCH)/lib

CFLAGS := -Wall -Wextra -O2 -I$(src)/include -I$(src)/include/uapi
SRC    := strela.c allocators.c $(src)/include/strela.h

.PHONY: all static shared clean

all: static shared

static: $(OUT)/libstrela.a
shared: $(OUT)/libstrela.so

$(OUT)/libstrela.a: $(SRC)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $(OUT)/strela.o strela.c
	$(AR) rcs $@ $(OUT)/strela.o

$(OUT)/libstrela.so: $(SRC)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ strela.c

clean:
	rm -f $(OUT)/libstrela.a $(OUT)/libstrela.so $(OUT)/strela.o
